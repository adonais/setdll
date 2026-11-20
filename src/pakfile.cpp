/*
 * 目标: 在 Edge About 设置页末尾追加内联 JS hook 或者 直接修改 Chrome About页面
 *       隐藏 "更新失败/错误" 行, 追加 "Portable msedge++" 品牌行.
 *
 * 核心方案（磁盘独有，区别于内存注入的FEXTRA原地回写）:
 *   1. 原 JS 一字节不改、不删除任何冗余代码
 *   2. 把 "原JS + 末尾hook" 重新 gzip 压缩, 长度 = new_sz
 *   3. 新数据从原槽位起点开始写回, 后面所有资源数据整体向后平移 (new_sz - old_sz) 字节
 *   4. 条目表中 offset > target_off 的条目, 其 offset 统一加上 delta
 *   5. 哨兵条目 offset 同步更新为新文件总长
 *   6. 别名表存的是 entry index, 不是 offset, 无需改动
 *
 * 特性:
 *   - 兼容 v4 / v5 Chrome 窄头 / v5 Edge 宽头 (自动探测)
 *   - 原 JS 一字节不动
 *   - 偏移保持单调非递减, pak 结构合法
 *   - 幂等（检测到 hook 标记直接跳过）
 *   - 默认备份为 .bak
 *
 * 编译(MinGW/Linux)
 *   gcc -O2 -c miniz.c      -o miniz.o
 *   gcc -O2 -c mini_gzip.c  -o mini_gzip.o
 *   g++ -O2 -static -o pak_patch_disk.exe pak_patch_disk.cpp miniz.o mini_gzip.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <optional>
#include <ranges>
#include <string_view>

#pragma warning (disable:4838)
#pragma warning (disable:4245)

// miniz.h 是"仅声明"版本（内部 define MINIZ_HEADER_FILE_ONLY 后 include miniz.c），
// 单独 include 它只为拿到 mz_deflate/mz_deflateInit2 等底层符号（-fast 分级压缩用），
// 必须包在 extern "C" 里以匹配 C 编译器产生的链接名。
#include "mini_gzip.h"
extern "C" {
#include "gzip/miniz.h"
}

static inline uint32_t R32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void W32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

struct PakLayout {
    bool     wide;
    uint32_t version;
    uint8_t  encoding;
    uint32_t count;
    uint32_t alias;
    size_t   hdr_len;
    size_t   entry_tbl_off;
    size_t   alias_tbl_off;
};

// Windows MSVC / macOS / BSD 等平台的 libc 没有 memmem, 此处提供一份可移植实现.
// Linux(glibc)、Android(bionic)、MinGW 自带, 直接用系统版本以避免重定义.
#if !defined(__GLIBC__) && !defined(__BIONIC__)
static const void *
memmem(const void *l, size_t l_len, const void *s, size_t s_len)
{
    char *cur, *last;
    const char *cl = (const char *)l;
    const char *cs = (const char *)s;

    /* we need something to compare */
    if (l_len == 0 || s_len == 0)
        return NULL;
 
    /* "s" must be smaller or equal to "l" */
    if (l_len < s_len)
        return NULL;
 
    /* special case where s_len == 1 */
    if (s_len == 1)
        return memchr(l, (int)*cs, l_len);
 
    /* the last position where its possible to find "s" in "l" */
    last = (char *)cl + l_len - s_len;
 
    for (cur = (char *)cl; cur <= last; cur++)
        if (cur[0] == cs[0] && memcmp(cur, cs, s_len) == 0)
            return cur;
 
    return NULL;
}
#endif

static std::vector<std::string> StringSplit(std::string_view str,
                                     const char delim,
                                     std::string_view enclosure = "") {
  std::vector<std::string> result;
  auto parts = std::views::split(str, delim);
  for (const auto& part : parts) {
    std::string_view part_sv(part);
    if (!enclosure.empty()) {
      if (!part_sv.empty() && part_sv.front() == enclosure.front()) {
        part_sv.remove_prefix(1);
      }
      if (!part_sv.empty() && part_sv.back() == enclosure.back()) {
        part_sv.remove_suffix(1);
      }
    }
    result.emplace_back(part_sv);
  }
  return result;
}

// Compression html.
static std::string& ltrim(std::string& s) {
  auto it = std::ranges::find_if_not(
      s, [](unsigned char c) { return std::isspace(c); });
  s.erase(s.begin(), it);
  return s;
}

static std::string& rtrim(std::string& s) {
  auto reversed_view = s | std::views::reverse;
  auto it = std::ranges::find_if_not(
      reversed_view, [](unsigned char c) { return std::isspace(c); });
  s.erase(it.base(), s.end());
  return s;
}

static std::string& trim(std::string& s) {
  return ltrim(rtrim(s));
}

static void compression_html(std::string& html) {
  auto lines = StringSplit(html, '\n');
  html.clear();
  for (auto& line : lines) {
    html += "\n";
    html += trim(line);
  }
}

static bool ReplaceStringInPlace(std::string& subject,
                          std::string_view search,
                          std::string_view replace) {
  bool find = false;
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::string::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
    find = true;
  }
  return find;
}

static bool DetectPak(const uint8_t* buf, size_t fsz, PakLayout* L) {
    if (fsz < 12) return false;
    uint32_t ver = R32(buf);
    if (ver != 4 && ver != 5) return false;
    L->version = ver;
    L->encoding = buf[4];

    if (ver == 4) {
        L->wide = false;
        L->hdr_len = 12;
        uint16_t k = *(const uint16_t*)(buf+8);
        L->count = k-1;
        L->alias = 0;
        L->entry_tbl_off = 12;
        size_t tend = 12 + (size_t)(L->count+1)*6;
        L->alias_tbl_off = tend;
        if (tend >= fsz) return false;
        uint32_t fo = R32(buf+12+2);
        if (fo != tend) return false;
        uint32_t pr = fo;
        for (uint32_t i=1;i<=L->count;i++){
            uint32_t o=R32(buf+12+i*6+2);
            if(o<pr) return false;
            pr=o;
        }
        return true;
    }
    for(int w=0;w<2;w++){
        size_t hl=w?16:12;
        if(fsz<hl) continue;
        uint32_t c=w?R32(buf+8):(uint32_t)*(const uint16_t*)(buf+8);
        uint32_t a=w?R32(buf+12):(uint32_t)*(const uint16_t*)(buf+10);
        if(c==0||c>500000) continue;
        if(a>500000) continue;
        size_t isz=w?8:6, asz=w?8:4;
        size_t ete=hl+(size_t)(c+1)*isz;
        size_t tend=ete+(size_t)a*asz;
        if(tend>=fsz) continue;
        uint32_t fo=R32(buf+hl+(w?4u:2u));
        if(fo!=tend) continue;
        bool ok=true; uint32_t pr=fo;
        for(uint32_t i=1;i<=c;i++){
            uint32_t o=R32(buf+hl+(size_t)i*isz+(w?4u:2u));
            if(o<pr){ok=false;break;}
            pr=o;
        }
        if(!ok) continue;
        for(uint32_t i=0;i<a;i++){
            uint32_t idx=w?R32(buf+ete+(size_t)i*asz+4)
                          :(uint32_t)*(const uint16_t*)(buf+ete+(size_t)i*asz+2);
            if(idx>=c){ok=false;break;}
        }
        if(!ok) continue;
        L->wide=!!w; L->hdr_len=hl; L->count=c; L->alias=a;
        L->entry_tbl_off=hl; L->alias_tbl_off=ete;
        return true;
    }
    return false;
}

static uint8_t* EntPtr(uint8_t* buf, const PakLayout& L, uint32_t i,
                       uint32_t* out_id, uint32_t* out_off) {
    size_t isz = L.wide?8:6;
    uint8_t* p = buf + L.entry_tbl_off + (size_t)i*isz;
    if(out_id)  *out_id  = L.wide?R32(p):(uint32_t)*(const uint16_t*)p;
    if(out_off) *out_off = R32(p + (L.wide?4:2));
    return p;
}

// ---- 标准 CRC-32（与 zlib 的 crc32(0, ...) 完全一致）----
// 不直接用迷你库里的 mz_crc32：它的累加器类型是 unsigned long，在 LP64 平台
// (Linux/macOS) 为 64 位，`crc = ~crc` 与 `crc >> 4` 会把高位垃圾移位污染进低
// 32 位，算出的并非标准 CRC-32（在 Windows LLP64 下 unsigned long 是 32 位，
// 反而恰好正确）。gzip 流末尾的 CRC 会被 Chromium/Edge 侧的 zlib 校验，一旦不符
// 资源就解压失败、补丁静默失效，所以这里统一自己算，保证任何平台产出都正确。
static uint32_t Crc32Std(const uint8_t* p, size_t n) {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- 解压：mini_gzip 的 mini_gz_start + mini_gz_unpack ----
// 传进来的 s/n 是**整条 gzip 流**（即整个槽位），末尾 4 字节就是该流的 ISIZE=明文长度。
static std::vector<uint8_t> GzDec(const uint8_t* s, size_t n) {
    std::vector<uint8_t> out;
    if (n < 18 || s[0]!=0x1F || s[1]!=0x8B || s[2]!=0x08) return out;
    uint32_t raw_len = R32(s + n - 4);                  // gzip ISIZE == 明文长度
    if (raw_len == 0 || raw_len > (64u<<20)) return out;
    struct mini_gzip gz;
    memset(&gz, 0, sizeof(gz));
    if (mini_gz_start(&gz, s, n) != 0) return out;
    // mini_gzip 的已知缺陷：mini_gz_start 只跳过 XLEN(2B)，没有跳过 FEXTRA 数据本身，
    // 此时 data_ptr 会指向填零区而不是 deflate 流。带 FEXTRA 的输入（本工具 FEXTRA
    // 回写产物）需要在这里手工把 data_ptr 前移 fextra_len 字节予以纠正。
    if ((s[3] & 0x04) != 0) {
        if ((size_t)gz.fextra_len > gz.data_len) return out;
        gz.data_ptr += gz.fextra_len;
        gz.data_len -= gz.fextra_len;
    }
    if (gz.data_len == 0) return out;
    // 注意：mini_gz_unpack 在错误返回前会 free(mem_out)，所以缓冲必须 malloc 分配，
    // 并且一旦返回负数就不能再 free 一次（否则 double free）。
    uint8_t* buf = (uint8_t*)malloc((size_t)raw_len + 64);
    if (!buf) return out;
    int got = mini_gz_unpack(&gz, buf, (size_t)raw_len + 64);
    if (got < 0) return out;                            // 缓冲已被库内 free
    if ((size_t)got != (size_t)raw_len) { free(buf); return out; }  // ISIZE 与实解不符 -> 数据可疑
    out.assign(buf, buf + (size_t)got);
    free(buf);
    return out;
}

// ---- 压缩：默认级别直接复用 mini_gzip 的 gzip_compress() ----
// mini_gzip 的 gzip_compress() 把级别写死为最高压缩级别、不能传参；为了让 -fast
// 仍然有效，非 9 级时改用同一套 miniz 后端手工拼 gzip 头/尾（产出格式与
// gzip_compress() 一致：FLG=0 的完整 gzip 流）。
static std::vector<uint8_t> GzEncMiniz(const uint8_t* s, size_t n, int level) {
    std::vector<uint8_t> v;
    size_t cap = 10 + n + (n >> 3) + 64 + 8;            // 原始 deflate 最坏情况的宽松上界
    v.resize(cap);
    static const uint8_t kGzHdr[10] = {0x1F,0x8B,0x08,0x00,0,0,0,0,0x00,0xFF};
    memcpy(v.data(), kGzHdr, 10);
    mz_stream z; memset(&z, 0, sizeof(z));
    if (mz_deflateInit2(&z, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9,
                        MZ_DEFAULT_STRATEGY) != MZ_OK) return {};
    z.next_in   = (const unsigned char*)s;
    z.avail_in  = (unsigned)n;
    z.next_out  = v.data() + 10;
    z.avail_out = (unsigned)(cap - 10);
    int r = mz_deflate(&z, MZ_FINISH);
    size_t dn = (size_t)z.total_out;
    mz_deflateEnd(&z);
    if (r != MZ_STREAM_END) return {};
    v.resize(10 + dn + 8);
    W32(v.data() + 10 + dn,     Crc32Std(s, n));                // CRC32（标准值）
    W32(v.data() + 10 + dn + 4, (uint32_t)(n & 0xFFFFFFFFu));    // ISIZE
    return v;
}

static std::vector<uint8_t> GzEnc(const uint8_t* s, size_t n, size_t* out_n, int lv=9) {
    std::vector<uint8_t> v;
    if (lv == 9) {
        // gzip_compress 返回值是 malloc 出来的，必须用 free 释放
        size_t olen = 0;
        void* p = gzip_compress(const_cast<uint8_t*>(s), n, &olen);
        if (!p) return v;
        v.assign((uint8_t*)p, (uint8_t*)p + olen);
        free(p);
        // gzip_compress() 内部用 mz_crc32 填 CRC，而 mz_crc32 在 LP64 平台会算错
        // （见上方 Crc32Std 的说明）。这里重算并回填末尾 8 字节，保证任何平台产出的
        // pak 都能通过 Chromium/Edge 侧 zlib 的 CRC 校验。
        if (v.size() >= 18) {
            W32(&v[v.size()-8], Crc32Std(s, n));
            W32(&v[v.size()-4], (uint32_t)(n & 0xFFFFFFFFu));
        }
    } else {
        v = GzEncMiniz(s, n, lv);
        if (v.empty()) return v;
    }
    if (out_n) *out_n = v.size();
    return v;
}

// ---- Edge 专属特征匹配：自动定位 PageDataService 目标资源 ----
// 不硬编码资源 ID。特征选用 Edge fork Chromium 后自己加的内部同步函数名
// innerUpdatePageDataFromRemote，它只出现在 PageDataService 核心模块里，
// Chrome 原版没有这个名字，误命中概率为 0。同时要求资源内包含 getPageData
// 作为双保险，避免未来某个版本碰巧出现相似字符串。
static const char kMarkerInner[] = "innerUpdatePageDataFromRemote";
static const char kMarkerGetPage[] = "/settings-about-page>";

// 返回值：命中的 entry 索引（不是资源 id）；找不到返回 (uint32_t)-1
// out_id / out_off / out_next 用于回填资源 id 和槽位起止 offset
static uint32_t FindEdgeTargetResourceId(const uint8_t* buf,
                                         const PakLayout& L,
                                         uint32_t* out_id,
                                         size_t*   out_off,
                                         size_t*   out_next,
                                         const bool ischorme) {
    for (uint32_t i=0; i<L.count; i++) {
        uint32_t id, off, nid, noff;
        EntPtr(const_cast<uint8_t*>(buf), L, i,   &id,  &off);
        EntPtr(const_cast<uint8_t*>(buf), L, i+1, &nid, &noff);
        size_t sz = (size_t)noff - (size_t)off;
        if (sz < 256) continue;                      // 太小不可能是核心模块（小于 256 字节的是小 chunk）
        const uint8_t* p = buf + off;
        if (p[0]!=0x1F || p[1]!=0x8B || p[2]!=0x08) continue;  // 不是 gzip
        // 槽位就是一条完整 gzip 流（末尾 4 字节是流自身的 ISIZE=明文长度），整段传入
        if (sz < 18) continue;
        std::vector<uint8_t> js = GzDec(p, sz);
        if (js.empty()) continue;

        const void* m1 = memmem(js.data(), js.size(), ischorme ? kMarkerGetPage: kMarkerInner, ischorme ? strlen(kMarkerGetPage) : strlen(kMarkerInner));
        if (m1) {
            if (out_id)   *out_id   = id;
            if (out_off)  *out_off  = off;
            if (out_next) *out_next = noff;
            return i;
        }
    }
    return (uint32_t)-1;
}

/*
 函数说明：对字符串中所有指定的子串进行替换
 参数：
string resource_str           //源字符串
string sub_str                //被替换子串
string new_str                //替换子串
返回值: string
*/
static std::string
subreplace(std::string resource_str, std::string sub_str, std::string new_str)
{
    std::string dst_str = resource_str;
    std::string::size_type pos = 0;
    while((pos = dst_str.find(sub_str)) != std::string::npos)   //替换所有指定子串
    {
        dst_str.replace(pos, sub_str.length(), new_str);
    }
    return dst_str;
}

static std::vector<uint8_t> dec_stream(const char *key) {
const unsigned char generate_resource[3029] = {
  '\x1F', '\x8B', '\x08', '\x08', '\xA6', '\x08', '\x00', '\x6B', '\x02', '\x03', '\x37', '\x31', '\x30', '\x32', '\x34', '\x00',
  '\xED', '\x59', '\x7D', '\x6F', '\xDB', '\xC6', '\x19', '\xFF', '\x2A', '\x0C', '\xBA', '\x8A', '\x64', '\x2D', '\x51', '\x92',
  '\xE3', '\x66', '\x85', '\x15', '\x26', '\x70', '\x6C', '\x2F', '\x33', '\xEA', '\xD8', '\x81', '\xAD', '\x36', '\x03', '\xEC',
  '\xAC', '\xA0', '\xC8', '\x93', '\x78', '\x31', '\x45', '\x32', '\xC7', '\xA3', '\x5E', '\x6A', '\x12', '\x48', '\xB2', '\x6E',
  '\x69', '\x81', '\xAE', '\x29', '\xB0', '\xAC', '\x6D', '\xB2', '\x02', '\x4D', '\xB7', '\xB6', '\x0B', '\x8A', '\x2E', '\x2D',
  '\xB6', '\x76', '\x4D', '\xD3', '\xA6', '\xF9', '\x30', '\x8B', '\x65', '\xFB', '\xAF', '\x7E', '\x85', '\x3D', '\x77', '\xC7',
  '\x37', '\xC9', '\x76', '\x1A', '\x6C', '\xDD', '\x7F', '\x43', '\x02', '\x8B', '\x7C', '\xEE', '\xB9', '\x7B', '\xDE', '\x7F',
  '\xF7', '\xDC', '\xB1', '\xA1', '\xB4', '\x43', '\xD7', '\xA4', '\xD8', '\x73', '\x15', '\x75', '\xBB', '\x67', '\x10', '\xC9',
  '\xD6', '\x9F', '\x0D', '\xB4', '\x0E', '\xA2', '\xE7', '\x8D', '\x0E', '\x5A', '\x30', '\xA8', '\x51', '\xB6', '\x83', '\x84',
  '\xB2', '\x8E', '\x0C', '\x62', '\xDA', '\x6B', '\x28', '\x08', '\x9D', '\x7C', '\xD4', '\x6B', '\x05', '\x88', '\xF4', '\x90',
  '\xA5', '\xBB', '\xA8', '\x2F', '\x5D', '\x40', '\xC6', '\xD6', '\x3A', '\xA2', '\x8A', '\x5A', '\x36', '\x6D', '\xC3', '\xED',
  '\x00', '\xB5', '\x56', '\xF6', '\x91', '\x6B', '\x61', '\xB7', '\x03', '\x4F', '\x8E', '\x11', '\xD0', '\x39', '\x6C', '\xE9',
  '\x95', '\x7A', '\xB9', '\x1D', '\x3A', '\xCE', '\x82', '\xE7', '\x22', '\xBD', '\xD6', '\x48', '\xC5', '\x4B', '\x5D', '\x83',
  '\x6C', '\x29', '\x48', '\xDD', '\xC6', '\x6D', '\xE5', '\x18', '\x62', '\xF2', '\xE6', '\x28', '\x25', '\xB8', '\x15', '\x52',
  '\xA4', '\xC8', '\x16', '\x88', '\xAA', '\x20', '\xAB', '\x83', '\x7C', '\x5F', '\x56', '\xD5', '\x6D', '\xA4', '\x05', '\x74',
  '\xE8', '\xC0', '\x5F', '\x50', '\x93', '\x78', '\x3E', '\x22', '\x74', '\x08', '\x3C', '\x38', '\xF0', '\x1D', '\x63', '\x28',
  '\x97', '\x65', '\x17', '\x56', '\x86', '\x1F', '\xDC', '\xF5', '\x3D', '\x42', '\x0D', '\x97', '\xCA', '\x6A', '\x83', '\xF3',
  '\x1E', '\xB1', '\x60', '\x59', '\xAE', '\x03', '\x47', '\xA2', '\xF1', '\xD4', '\x54', '\x1C', '\x67', '\x2A', '\x59', '\x08',
  '\xF9', '\xF3', '\x9E', '\xE3', '\x20', '\x93', '\x2A', '\xC4', '\xF3', '\x68', '\xD9', '\x30', '\x4D', '\x75', '\x9B', '\x3D',
  '\x69', '\x97', '\x43', '\x44', '\x86', '\xEB', '\x88', '\x0D', '\x79', '\x64', '\xCE', '\x71', '\x14', '\xF9', '\x39', '\x59',
  '\xD5', '\xDA', '\x1E', '\x59', '\x34', '\x4C', '\x3B', '\xF7', '\x29', '\xD8', '\x03', '\x73', '\x34', '\x3F', '\x0C', '\x6C',
  '\x78', '\x6E', '\x80', '\x6D', '\xA0', '\x89', '\x6D', '\x58', '\x5E', '\x7F', '\x0D', '\x56', '\x51', '\x8B', '\x02', '\x8A',
  '\x03', '\x5C', '\x50', '\xAC', '\x36', '\x08', '\xA2', '\x21', '\x71', '\x25', '\x78', '\xCB', '\x95', '\x4A', '\x3C', '\xCE',
  '\xD8', '\x14', '\x22', '\xDC', '\x45', '\xA2', '\x28', '\x8D', '\x83', '\x66', '\x1B', '\x01', '\x90', '\x55', '\x31', '\xB3',
  '\x91', '\x91', '\x0D', '\xCB', '\x02', '\x72', '\x83', '\x45', '\xE9', '\x5C', '\x48', '\x0D', '\xB6', '\xD2', '\xAA', '\x18',
  '\x23', '\x8A', '\xE7', '\x72', '\x12', '\x52', '\xB5', '\x84', '\x5D', '\x21', '\xE5', '\x6D', '\xD3', '\xC6', '\x8E', '\xB5',
  '\x8C', '\x03', '\x3A', '\x4B', '\x49', '\x88', '\xCA', '\x41', '\xD8', '\xA2', '\x04', '\x21', '\xF1', '\x02', '\xBE', '\x22',
  '\x86', '\x49', '\x11', '\x61', '\x29', '\xC0', '\x49', '\xB1', '\x9A', '\x2B', '\xD8', '\x0A', '\x61', '\xE6', '\x79', '\xAF',
  '\x8F', '\x08', '\xB2', '\x14', '\xB3', '\x2F', '\xD2', '\xCA', '\xD7', '\x2D', '\xCF', '\x0C', '\xBB', '\xC8', '\xA5', '\x9A',
  '\x49', '\x10', '\x08', '\x5B', '\x74', '\x10', '\x7B', '\x63', '\x71', '\xEB', '\x81', '\xFB', '\xFD', '\x89', '\x00', '\x99',
  '\x90', '\x2D', '\x01', '\x84', '\x46', '\xC4', '\xA8', '\xE2', '\x8B', '\xE5', '\x04', '\x23', '\x8F', '\x7D', '\x87', '\x60',
  '\x0B', '\x5C', '\x17', '\x76', '\x5D', '\x5D', '\xAE', '\x4B', '\x55', '\xA9', '\x52', '\x97', '\xB3', '\xB1', '\x3E', '\xB6',
  '\xA8', '\x0D', '\xE4', '\x5A', '\xED', '\xD9', '\x9C', '\x08', '\xF9', '\xD5', '\xC1', '\x6E', '\xD3', '\xF3', '\x75', '\xF9',
  '\x05', '\x7F', '\x90', '\xD3', '\xDB', '\x9E', '\x4B', '\xD7', '\xF1', '\xAB', '\x08', '\xF8', '\xA7', '\x8B', '\x74', '\xD3',
  '\x73', '\x3C', '\xA2', '\xCB', '\xCF', '\x9C', '\x40', '\xEC', '\x1F', '\xA3', '\x1B', '\x3E', '\x4B', '\xE6', '\x79', '\xE6',
  '\x18', '\x65', '\xC2', '\x9A', '\x26', '\x1A', '\xD0', '\x15', '\xCF', '\x02', '\xC5', '\x13', '\xC3', '\xA5', '\xD6', '\x50',
  '\x82', '\x54', '\x6D', '\x30', '\xDB', '\x8D', '\x23', '\x6D', '\x37', '\xC0', '\x20', '\x63', '\xC2', '\x72', '\x9B', '\xA0',
  '\x36', '\x18', '\x6E', '\x53', '\xEA', '\x07', '\xB3', '\xD5', '\x6A', '\x07', '\x53', '\x3B', '\x6C', '\x81', '\x36', '\xDD',
  '\x2A', '\x64', '\x87', '\x6B', '\xE0', '\xA0', '\xEA', '\xE0', '\x16', '\xCF', '\xEC', '\x96', '\x83', '\xC4', '\xFC', '\x31',
  '\x7D', '\x6B', '\xB5', '\x9F', '\xBF', '\x60', '\xCD', '\xC8', '\x19', '\x9D', '\x82', '\x6A', '\x0B', '\xC8', '\xF4', '\x08',
  '\x0F', '\xBA', '\x2E', '\xAA', '\x03', '\x46', '\x19', '\x7D', '\x1E', '\x6C', '\x07', '\x45', '\x74', '\xD0', '\x5A', '\xAC',
  '\x27', '\x75', '\x03', '\xE6', '\xF1', '\xA9', '\xA9', '\x49', '\x7B', '\x0D', '\xF5', '\x69', '\x1D', '\x20', '\xD5', '\xB5',
  '\xE3', '\x5A', '\x4D', '\x52', '\xC0', '\x01', '\x89', '\xC2', '\x2A', '\x73', '\x84', '\xD9', '\x1F', '\x9B', '\xEE', '\x17',
  '\x2B', '\x2E', '\x4B', '\x9D', '\x57', '\xED', '\x17', '\xB1', '\x6B', '\x25', '\x48', '\x64', '\xE8', '\x1B', '\x17', '\xCB',
  '\x18', '\x20', '\x82', '\x92', '\x21', '\x4B', '\x73', '\xD7', '\xE8', '\xE1', '\x8E', '\x01', '\xE5', '\xA6', '\x39', '\x30',
  '\x31', '\x04', '\x04', '\x0A', '\x4A', '\xA5', '\x43', '\x88', '\x9A', '\x83', '\xDC', '\x0E', '\xB5', '\x55', '\x28', '\x46',
  '\x85', '\x4D', '\xC7', '\x27', '\x8F', '\x66', '\x6A', '\xE0', '\xA9', '\x29', '\xD5', '\x10', '\xF5', '\x79', '\x08', '\xD7',
  '\x06', '\xBE', '\xA8', '\xC6', '\xA6', '\x41', '\x4D', '\x56', '\xBD', '\xDB', '\xF1', '\x91', '\x8A', '\x1C', '\xBD', '\xC4',
  '\x13', '\xA7', '\x87', '\x50', '\x6D', '\xCB', '\x47', '\x2E', '\x31', '\x36', '\x7A', '\x70', '\x99', '\x2C', '\x00', '\xE9',
  '\x43', '\x92', '\x55', '\xA5', '\xD2', '\x51', '\x23', '\x5C', '\xA9', '\x54', '\xCE', '\x93', '\x99', '\x0A', '\xE2', '\x58',
  '\x28', '\xA8', '\x6E', '\x68', '\x97', '\x3C', '\xEC', '\x2A', '\x90', '\x98', '\xAA', '\x46', '\xBD', '\x65', '\x96', '\xE4',
  '\xF3', '\x46', '\x80', '\x14', '\x8E', '\x69', '\x54', '\x0B', '\x1C', '\x6C', '\x22', '\xA5', '\x56', '\x9E', '\x56', '\x75',
  '\x5D', '\x97', '\x91', '\x2B', '\x27', '\x00', '\x24', '\xD5', '\x26', '\xC7', '\x8F', '\xC1', '\xF8', '\xAB', '\x76', '\x61',
  '\x9C', '\xAD', '\x1F', '\xE8', '\x39', '\xCB', '\xF3', '\x7C', '\xC9', '\x40', '\xE7', '\x7C', '\x15', '\xD3', '\x95', '\xA3',
  '\x28', '\x7D', '\x09', '\x3A', '\xD9', '\xBC', '\x7A', '\x8A', '\x8D', '\xD3', '\x07', '\x80', '\x47', '\x64', '\x72', '\x13',
  '\xFB', '\x8A', '\xE1', '\x9A', '\xB6', '\x47', '\x7E', '\x22', '\x00', '\x12', '\xCB', '\x56', '\x28', '\xF6', '\x0B', '\x28',
  '\x94', '\x6C', '\x3A', '\xBA', '\xDC', '\x72', '\x3C', '\x73', '\xEB', '\xA9', '\x10', '\x48', '\xE0', '\x8C', '\x54', '\xCB',
  '\xE9', '\x3E', '\x80', '\x33', '\xDB', '\x22', '\xC5', '\x40', '\xFD', '\x44', '\x11', '\x85', '\x5A', '\x1E', '\xB1', '\x10',
  '\x59', '\x33', '\x2C', '\x1C', '\x06', '\xBA', '\x3C', '\x73', '\x04', '\x70', '\x1D', '\x2F', '\xD2', '\x1D', '\xEC', '\xA2',
  '\x5F', '\x22', '\xDC', '\xB1', '\xA1', '\xAE', '\xEB', '\xDA', '\xF3', '\x4F', '\x42', '\xB4', '\x54', '\xC6', '\x00', '\xD6',
  '\xE1', '\x1A', '\x08', '\x71', '\x15', '\xA0', '\xC8', '\x3C', '\x30', '\x5B', '\x7A', '\x5A', '\x92', '\xC0', '\x5D', '\x44',
  '\x8C', '\x2D', '\x08', '\x49', '\xED', '\xB4', '\xDC', '\xB4', '\x71', '\x20', '\xC1', '\x7F', '\x6A', '\x23', '\x29', '\x75',
  '\x91', '\x04', '\x7B', '\x4A', '\xC0', '\x77', '\xAA', '\xB6', '\xB4', '\x08', '\xDE', '\x2B', '\x4B', '\x43', '\x2F', '\x94',
  '\x5C', '\x04', '\xA0', '\x48', '\x3D', '\x09', '\xB2', '\x5A', '\x32', '\x80', '\x1F', '\x13', '\xAB', '\xE2', '\x1B', '\xB0',
  '\x73', '\x03', '\xD1', '\x73', '\xD8', '\x08', '\xF8', '\x5F', '\xC2', '\x54', '\x32', '\xC4', '\x6A', '\x16', '\x6A', '\x1B',
  '\xD0', '\x68', '\x48', '\x2D', '\xE2', '\xF5', '\xA1', '\x10', '\x34', '\x69', '\x3D', '\xEC', '\x40', '\x45', '\xD2', '\x59',
  '\x49', '\x9E', '\x55', '\x98', '\xF4', '\xE9', '\xD3', '\xF2', '\xFE', '\x95', '\x5B', '\xA3', '\xF7', '\x3E', '\xE7', '\x42',
  '\xA4', '\xC7', '\xDF', '\x3F', '\x1A', '\xDD', '\x7C', '\x7F', '\xF7', '\x8D', '\xD7', '\x47', '\xEF', '\x7F', '\xF6', '\xC3',
  '\x77', '\x6F', '\x3E', '\x7E', '\x78', '\x67', '\xFF', '\xFD', '\x2B', '\x7B', '\x9F', '\x5C', '\x7D', '\xFC', '\xF0', '\xD1',
  '\xEE', '\xCD', '\xBB', '\xBB', '\x9F', '\x7D', '\xF6', '\xF8', '\xFE', '\x1B', '\xA3', '\x77', '\xBE', '\xD9', '\xF9', '\xFA',
  '\xE3', '\x9D', '\xDF', '\x7E', '\xBD', '\xF3', '\xC5', '\xF5', '\x9D', '\x7B', '\xBF', '\xD9', '\xBB', '\xFB', '\xB7', '\x9D',
  '\x7B', '\xB7', '\x77', '\xAF', '\x3D', '\xD8', '\xBF', '\xF3', '\x36', '\x3C', '\xEF', '\x5E', '\xB9', '\xB1', '\xF7', '\xC9',
  '\xC3', '\x9D', '\x5B', '\x77', '\xFF', '\x75', '\xE5', '\x9A', '\x34', '\x7A', '\xEB', '\xEE', '\xDE', '\x3B', '\x9F', '\xFC',
  '\xF0', '\xDD', '\x6D', '\x10', '\x27', '\xEF', '\x3D', '\x1A', '\x97', '\xF3', '\xF6', '\x83', '\xA7', '\x96', '\xF3', '\x3B',
  '\x26', '\xE7', '\xDE', '\xF7', '\xBB', '\x0F', '\xEF', '\x3D', '\xBE', '\xFF', '\x60', '\xFF', '\xDB', '\xF7', '\xF6', '\xEE',
  '\x7D', '\x34', '\xFA', '\xEA', '\xC6', '\xDE', '\x5F', '\x5F', '\x2F', '\xC8', '\xF9', '\xFD', '\xDB', '\x5C', '\xCE', '\x4F',
  '\xBB', '\x59', '\xAC', '\xE3', '\xE5', '\xE3', '\x2B', '\xF3', '\xC7', '\xAB', '\x69', '\x3D', '\xAC', '\xA1', '\x0E', '\x6C',
  '\xE6', '\x84', '\x21', '\xCB', '\x4F', '\xBD', '\x69', '\x14', '\x97', '\x3E', '\xB8', '\x6D', '\x88', '\x22', '\xD4', '\xB0',
  '\x0B', '\x51', '\xA4', '\x73', '\xD6', '\x25', '\xC3', '\xCC', '\xC1', '\x06', '\x8C', '\x6A', '\x43', '\x1B', '\x01', '\xEC',
  '\x72', '\xF9', '\xF0', '\x1D', '\x81', '\xF5', '\x81', '\xF0', '\xD4', '\x43', '\x4B', '\xE9', '\xB6', '\x10', '\xEA', '\x7D',
  '\x48', '\x48', '\xAF', '\xAF', '\x41', '\xC5', '\x71', '\xED', '\x34', '\xE6', '\x04', '\x86', '\x19', '\xD5', '\x8D', '\xD3',
  '\xA5', '\x67', '\x2E', '\x06', '\xBC', '\x4D', '\xD5', '\xAB', '\x18', '\x94', '\x0D', '\xA8', '\x12', '\xAA', '\x51', '\x24',
  '\x06', '\x78', '\xCB', '\x56', '\xA0', '\xA7', '\x50', '\x32', '\x3D', '\xC3', '\x27', '\x6F', '\x56', '\x6D', '\xE4', '\xF8',
  '\x9B', '\xAD', '\x68', '\xB3', '\x6A', '\xB4', '\xBC', '\x90', '\xC2', '\x93', '\x0F', '\xC8', '\xBB', '\x64', '\xE9', '\xFC',
  '\x35', '\x7D', '\x61', '\x4C', '\x87', '\xAC', '\x51', '\x4F', '\xD6', '\x48', '\xD2', '\xF6', '\x8C', '\xC8', '\xDA', '\x7C',
  '\x8D', '\x71', '\xFA', '\xC1', '\x05', '\xEA', '\x33', '\x22', '\xFA', '\xD3', '\xF5', '\x3C', '\xFE', '\x63', '\x3D', '\xA6',
  '\x22', '\x43', '\xE4', '\x29', '\x14', '\x69', '\x50', '\xE1', '\xFA', '\x54', '\xD8', '\xC2', '\xB2', '\x5A', '\x36', '\xEA',
  '\x33', '\x3F', '\x3E', '\x23', '\x91', '\x5E', '\x49', '\x8A', '\x29', '\x99', '\x9B', '\xF7', '\xDE', '\x3D', '\x1C', '\x64',
  '\xAD', '\xF7', '\x38', '\x32', '\x13', '\x9D', '\xF7', '\xE2', '\x67', '\xBC', '\x90', '\xB7', '\xF1', '\xF3', '\x0E', '\x06',
  '\x39', '\x6B', '\xAC', '\x5F', '\xE5', '\x30', '\x4D', '\x04', '\xD2', '\x9D', '\xD4', '\x6B', '\x51', '\x44', '\x34', '\x9B',
  '\xA3', '\x0E', '\xBC', '\x8C', '\xA1', '\x3F', '\xD2', '\xBC', '\x76', '\x9B', '\xB5', '\xEA', '\x06', '\x04', '\x9A', '\x02',
  '\xFE', '\xBB', '\xD0', '\xFD', '\xE7', '\x38', '\xCE', '\xA4', '\x98', '\x81', '\x0E', '\x42', '\xE6', '\xBD', '\xAE', '\x0F',
  '\x59', '\x6D', '\xAD', '\xB3', '\x14', '\x64', '\xED', '\x72', '\xC2', '\x63', '\x06', '\x9A', '\xEF', '\x05', '\x98', '\x67',
  '\x22', '\x6C', '\x04', '\x6D', '\x3C', '\x80', '\x4E', '\x30', '\x8A', '\x80', '\x9C', '\xC0', '\x2F', '\xDB', '\x53', '\x78',
  '\x86', '\x9E', '\xAE', '\xCF', '\xD6', '\x62', '\x10', '\xC9', '\xEC', '\x01', '\x57', '\x4E', '\x84', '\x87', '\x53', '\xEB',
  '\x33', '\x45', '\x9F', '\x03', '\x95', '\xF1', '\x8D', '\xB1', '\x31', '\x96', '\x9C', '\x23', '\xB5', '\x24', '\x4F', '\x49',
  '\x12', '\xBA', '\xEB', '\x7D', '\x68', '\xDC', '\x15', '\xE4', '\x04', '\x65', '\x03', '\x5B', '\xEA', '\x76', '\x76', '\xDA',
  '\xC9', '\x3D', '\x4A', '\x07', '\x94', '\x79', '\x54', '\xCC', '\x56', '\x50', '\xB1', '\x70', '\xA2', '\x48', '\x86', '\x4D',
  '\x94', '\x20', '\xD0', '\x1C', '\x36', '\xBC', '\xEA', '\x66', '\x30', '\x55', '\xED', '\x94', '\x65', '\x89', '\x6D', '\xAC',
  '\x04', '\x77', '\x15', '\x95', '\x19', '\x00', '\xCB', '\x32', '\x88', '\xAB', '\x47', '\x51', '\xF2', '\x34', '\x23', '\x92',
  '\x1F', '\xA0', '\x75', '\x0D', '\x42', '\x08', '\x9D', '\x51', '\x83', '\xB5', '\x37', '\x8C', '\x24', '\x5A', '\x1C', '\xD0',
  '\xA5', '\xD8', '\xD2', '\x70', '\x66', '\xA4', '\x03', '\x15', '\x5A', '\x18', '\x11', '\x04', '\x6A', '\x74', '\x56', '\x8C',
  '\x2E', '\x62', '\xBE', '\x5A', '\x6F', '\xCE', '\xAD', '\x2C', '\xCC', '\xAD', '\x2D', '\x54', '\xD6', '\x56', '\x2F', '\x54',
  '\xE6', '\x57', '\x57', '\x9A', '\x73', '\x4B', '\x2B', '\x8B', '\x6B', '\xB2', '\x6A', '\x82', '\x82', '\xD8', '\x0D', '\x51',
  '\xE3', '\x47', '\xCF', '\x60', '\xA5', '\x92', '\x22', '\x2C', '\x1C', '\x6F', '\x06', '\xA0', '\xCA', '\x2D', '\x34', '\x58',
  '\x6D', '\x03', '\xB7', '\xD7', '\x77', '\x1D', '\xCF', '\xB0', '\xA4', '\xD0', '\x87', '\x89', '\x28', '\x90', '\x3C', '\x50',
  '\x5D', '\x66', '\xDD', '\x01', '\x64', '\x4A', '\x32', '\x35', '\x63', '\xDE', '\xBF', '\x72', '\x7B', '\xEF', '\xD1', '\xF5',
  '\xD1', '\x9B', '\x6F', '\x8C', '\xBE', '\xBA', '\xBA', '\x7F', '\xFD', '\xC6', '\xDE', '\xBD', '\x0F', '\xF7', '\xBE', '\xFC',
  '\x66', '\xF7', '\xF6', '\x6B', '\x47', '\xF3', '\xDF', '\xD8', '\xBF', '\xFA', '\xD6', '\xDE', '\xDD', '\xD7', '\x81', '\xF9',
  '\x31', '\x60', '\xEA', '\xDF', '\xBF', '\x15', '\x9C', '\xAA', '\x9A', '\x1C', '\x22', '\x79', '\x31', '\x2A', '\x02', '\xA4',
  '\xA3', '\x97', '\xC5', '\x76', '\xA4', '\x6E', '\x06', '\xCF', '\x6D', '\x5A', '\x53', '\x9B', '\xDA', '\xD8', '\x9F', '\xB4',
  '\x12', '\x0F', '\x46', '\x48', '\x4D', '\x7C', '\x9D', '\x1E', '\xDE', '\xE2', '\xD4', '\xE1', '\x3D', '\x70', '\x78', '\xEF',
  '\x64', '\x3A', '\x9A', '\x38', '\xBD', '\x97', '\x3A', '\xDD', '\xD4', '\x93', '\x91', '\x8D', '\xDE', '\x45', '\x9E', '\xDB',
  '\x00', '\x7C', '\xD8', '\x42', '\x7A', '\xF1', '\xA8', '\x67', '\x96', '\x37', '\x2E', '\x0A', '\xB4', '\x87', '\xBD', '\x7A',
  '\x8B', '\x45', '\xB3', '\x6C', '\xF6', '\x79', '\x59', '\x64', '\x51', '\xDD', '\x02', '\x21', '\x5B', '\x27', '\xC5', '\xDC',
  '\x54', '\xC6', '\x56', '\x2A', '\x63', '\xA0', '\x8B', '\x81', '\x8D', '\x2D', '\x21', '\x82', '\xBA', '\xFA', '\x20', '\x8D',
  '\x2F', '\xEF', '\xB7', '\x78', '\x9D', '\xAC', '\x2F', '\x36', '\x9B', '\x4B', '\x2B', '\x67', '\xD7', '\x2B', '\xCD', '\xC5',
  '\x73', '\xE7', '\x97', '\xE7', '\x9A', '\x8B', '\x95', '\xE5', '\xA5', '\x95', '\x17', '\x65', '\x5E', '\xE6', '\x83', '\xC2',
  '\x61', '\xB3', '\x54', '\xAA', '\x2A', '\xA3', '\xBF', '\x5C', '\x19', '\x7D', '\xF0', '\xF1', '\xE8', '\x4F', '\x5F', '\x8E',
  '\xDE', '\xF9', '\x62', '\xF4', '\xEE', '\x3F', '\x77', '\xAE', '\x3F', '\xD8', '\xBF', '\x79', '\x2B', '\x1A', '\x7D', '\xFA',
  '\xE7', '\x9C', '\x7A', '\xEB', '\xDA', '\xEE', '\xAD', '\xEF', '\x76', '\x6F', '\x7E', '\xB0', '\xFF', '\xE6', '\xE7', '\x7B',
  '\x9F', '\x7E', '\x14', '\xCD', '\xB9', '\x12', '\x22', '\xC4', '\x23', '\x92', '\x67', '\x9A', '\x21', '\x61', '\x27', '\x9E',
  '\x3E', '\x20', '\x3E', '\x92', '\x4C', '\x1B', '\x99', '\x5B', '\x80', '\x14', '\x6A', '\xEA', '\xD9', '\xA2', '\xA8', '\xA2',
  '\x93', '\xE1', '\x1C', '\xCF', '\xA3', '\x35', '\x50', '\x1B', '\xDC', '\x0B', '\xC2', '\xCF', '\x03', '\x35', '\x8E', '\x21',
  '\x6B', '\x91', '\x94', '\x99', '\xB1', '\xB0', '\xF4', '\xB2', '\x9C', '\xB8', '\x96', '\x99', '\xC9', '\xFB', '\x32', '\x66',
  '\x28', '\x0B', '\x12', '\x8F', '\x33', '\xC5', '\x14', '\xFA', '\x32', '\x0C', '\xC9', '\x5B', '\xE9', '\x13', '\xC3', '\xAF',
  '\x0A', '\xB1', '\xA6', '\x9B', '\x64', '\x03', '\xAC', '\x9F', '\x2E', '\x08', '\x4E', '\x16', '\xE8', '\x03', '\x26', '\x9B',
  '\x42', '\x8B', '\xC9', '\x29', '\xC0', '\x32', '\x88', '\xB3', '\x50', '\x6F', '\x4D', '\xB3', '\x30', '\x4C', '\x4F', '\xC6',
  '\x61', '\x3A', '\x0D', '\xC4', '\x30', '\x0B', '\xC4', '\x34', '\xAF', '\xB2', '\x61', '\xB1', '\xCA', '\xB8', '\xE2', '\x63',
  '\x25', '\x55', '\x55', '\x7E', '\x1D', '\x6D', '\x06', '\xAA', '\x50', '\x98', '\x09', '\x56', '\x36', '\x83', '\xE8', '\x67',
  '\x6A', '\x22', '\x7E', '\x38', '\x6E', '\x9B', '\x9A', '\xCF', '\x4D', '\xD5', '\x71', '\x58', '\xAD', '\x3B', '\xF8', '\xA4',
  '\xF0', '\x57', '\xA2', '\x8D', '\xC3', '\xEB', '\x9D', '\x0B', '\x67', '\x13', '\x0C', '\xD0', '\x48', '\xE1', '\x0C', '\x1B',
  '\x0E', '\x9C', '\x5E', '\x12', '\x27', '\x0F', '\xD5', '\x46', '\x0B', '\xBA', '\x89', '\xAD', '\x38', '\x8E', '\xB9', '\x1F',
  '\x4A', '\xA5', '\x63', '\x70', '\x20', '\x9B', '\xD8', '\x2E', '\xB4', '\xC9', '\xC3', '\xB6', '\x3A', '\x79', '\x94', '\x8F',
  '\x73', '\x6C', '\xAA', '\xCF', '\x4C', '\x62', '\x93', '\x69', '\xB8', '\x06', '\x19', '\x2E', '\x3A', '\xE3', '\x89', '\x7C',
  '\x19', '\x54', '\xBE', '\x5C', '\x84', '\xA7', '\xCB', '\x59', '\xA5', '\x08', '\x7C', '\xBA', '\x9C', '\xE4', '\xB0', '\xCE',
  '\xAA', '\xDC', '\x4C', '\x6B', '\x77', '\xF4', '\xEE', '\x9D', '\xD1', '\x3F', '\xFE', '\x08', '\xFD', '\x53', '\xB4', '\xFB',
  '\xDA', '\x87', '\xFC', '\xE9', '\xBA', '\xAA', '\x6D', '\xC3', '\x01', '\xA2', '\x16', '\xCF', '\x73', '\x39', '\xC9', '\x8B',
  '\x02', '\xCD', '\xD5', '\xC1', '\xCE', '\x2A', '\x02', '\x44', '\xD9', '\xBF', '\xFA', '\x07', '\xD6', '\xD4', '\x5D', '\x7B',
  '\xA0', '\x46', '\xF9', '\x8C', '\x19', '\x98', '\x01', '\x7A', '\xCA', '\x34', '\x82', '\xBF', '\xAE', '\x47', '\x55', '\xA9',
  '\x05', '\x67', '\x5E', '\x03', '\x62', '\x2B', '\x96', '\x9B', '\xE8', '\x3A', '\xC7', '\x67', '\x8E', '\xEE', '\xDF', '\x81',
  '\x45', '\xD9', '\x4B', '\xBD', '\x16', '\x4F', '\x48', '\x4C', '\xB3', '\x9D', '\xAA', '\xCC', '\xB5', '\x3F', '\x72', '\x7F',
  '\xC5', '\xC1', '\xD9', '\xD1', '\xCD', '\x3C', '\xB2', '\xA6', '\x83', '\xBB', '\x2D', '\xF0', '\x14', '\xFF', '\x3D', '\x79',
  '\x42', '\xFC', '\x26', '\x71', '\x3D', '\x86', '\x1C', '\x38', '\x22', '\x90', '\xBC', '\x6B', '\x8A', '\xA2', '\x49', '\x0A',
  '\x44', '\x21', '\xEB', '\x01', '\x5A', '\x9E', '\x35', '\x54', '\x79', '\xB0', '\x1B', '\xE2', '\xDC', '\x33', '\xC9', '\x2C',
  '\x76', '\x5C', '\xD6', '\x9E', '\x2D', '\x3A', '\xC1', '\x18', '\x2E', '\xF9', '\x19', '\x2E', '\xD9', '\x46', '\x70', '\x06',
  '\xCA', '\x2F', '\xE5', '\xD2', '\x02', '\xAF', '\x8B', '\xF2', '\x5B', '\xAD', '\x41', '\xBA', '\xB1', '\x49', '\x19', '\xEC',
  '\x8C', '\x01', '\xCE', '\x99', '\x97', '\x9A', '\xCD', '\xD5', '\x15', '\xB6', '\xBB', '\xC0', '\x36', '\x3D', '\xC6', '\x32',
  '\xBF', '\x96', '\x0C', '\x4E', '\x0E', '\x1C', '\x4E', '\x9D', '\x3B', '\x64', '\x3E', '\x03', '\x31', '\xBE', '\x74', '\x2C',
  '\x54', '\xED', '\xEB', '\xFE', '\xE1', '\x1D', '\xCA', '\xE9', '\x73', '\x06', '\xB5', '\xB5', '\xB6', '\xE3', '\x81', '\x8F',
  '\xFD', '\xA3', '\x9A', '\x18', '\xD1', '\xBF', '\xA8', '\xB3', '\xA2', '\xDB', '\xC1', '\x41', '\xD3', '\xF3', '\x2E', '\x30',
  '\xB8', '\xEE', '\x9F', '\xAA', '\x95', '\x4A', '\xFD', '\x53', '\x85', '\x25', '\x92', '\xAE', '\x13', '\xBB', '\x2E', '\x22',
  '\x17', '\xD8', '\xA4', '\xE7', '\x6A', '\xDA', '\x89', '\xCC', '\x5B', '\xAB', '\x70', '\x66', '\x21', '\xEB', '\x88', '\xFB',
  '\x27', '\xF8', '\x4F', '\xFD', '\xD6', '\x5C', '\x3D', '\x7B', '\x76', '\x79', '\x71', '\xD2', '\xE6', '\x6C', '\x78', '\x7E',
  '\xF5', '\xDC', '\x99', '\xD5', '\x33', '\xAB', '\xBF', '\x3A', '\xC0', '\x70', '\xF8', '\x76', '\x7E', '\xD4', '\x32', '\x6B',
  '\x73', '\x0B', '\x4B', '\xAB', '\x95', '\xB3', '\x6B', '\xAB', '\x2F', '\x9D', '\x67', '\x3E', '\x84', '\x0C', '\x13', '\xD1',
  '\x86', '\xC4', '\xCD', '\xEC', '\x87', '\xE7', '\x49', '\xA3', '\xD4', '\x6D', '\x48', '\x59', '\x3F', '\x81', '\x11', '\xF6',
  '\x18', '\x67', '\x05', '\x8F', '\x1C', '\x01', '\x2B', '\xC9', '\x7B', '\x82', '\x39', '\xD9', '\x2B', '\xC7', '\xBE', '\xF4',
  '\x4D', '\x73', '\x61', '\x0B', '\x48', '\x32', '\x71', '\x1D', '\xB7', '\x00', '\xA9', '\x3A', '\x51', '\xF4', '\xA4', '\x51',
  '\x81', '\x8A', '\xEC', '\x26', '\xF2', '\x69', '\xF9', '\x72', '\x1C', '\x3C', '\xFC', '\xF4', '\xAE', '\x1E', '\xB8', '\x28',
  '\xC8', '\x54', '\x8D', '\xE3', '\xB4', '\xE7', '\x14', '\x2D', '\x5D', '\xE1', '\xDA', '\x35', '\xB9', '\x24', '\x55', '\xBA',
  '\x21', '\x0D', '\x78', '\x5D', '\x26', '\x37', '\xDB', '\xE9', '\x55', '\x6B', '\x7A', '\xD1', '\x2D', '\x3A', '\x5A', '\x68',
  '\x12', '\xF5', '\xC2', '\xCD', '\x7A', '\x76', '\x0B', '\xCE', '\x7C', '\xC1', '\x56', '\x28', '\x95', '\xD2', '\x3B', '\x70',
  '\x01', '\x07', '\xD0', '\x1F', '\xB3', '\x36', '\x20', '\x40', '\xC8', '\x1D', '\xBF', '\x4A', '\x9F', '\xE8', '\xF3', '\xD8',
  '\xDC', '\x03', '\x8D', '\x1E', '\xD5', '\x19', '\x19', '\x3A', '\x3D', '\x88', '\x37', '\x81', '\x44', '\xE7', '\x0E', '\x67',
  '\xEE', '\xA2', '\x69', '\x02', '\xE4', '\x7B', '\x09', '\xE3', '\x77', '\x75', '\xAA', '\xB9', '\x9E', '\x85', '\x9A', '\x43',
  '\x9F', '\xA5', '\x46', '\xFD', '\x34', '\x9D', '\xA5', '\x13', '\x18', '\xC1', '\x56', '\x70', '\xA3', '\x88', '\xE9', '\xC3',
  '\xAF', '\x94', '\xDD', '\xC2', '\x76', '\xC4', '\x89', '\xEC', '\x42', '\xD9', '\x55', '\xE1', '\x99', '\x26', '\xF7', '\x56',
  '\x6A', '\xA3', '\x08', '\x25', '\x2E', '\x98', '\x42', '\x79', '\x2B', '\xCB', '\x38', '\x92', '\x1B', '\xB9', '\xED', '\xAC',
  '\x75', '\x06', '\x62', '\xFA', '\x3D', '\x40', '\x6D', '\x30', '\xE5', '\xC5', '\xA6', '\x21', '\x5C', '\x19', '\xF3', '\x3B',
  '\x27', '\xB6', '\xB9', '\x8C', '\x9D', '\xFE', '\x1A', '\xE9', '\x07', '\x04', '\x18', '\x11', '\x87', '\x25', '\xC7', '\x19',
  '\x83', '\xAF', '\x14', '\x02', '\x39', '\x8A', '\x65', '\xA2', '\x80', '\x8B', '\x77', '\xE9', '\x05', '\x31', '\xD9', '\xE7',
  '\x87', '\x7A', '\xCC', '\x0C', '\x4D', '\x2A', '\x9B', '\x20', '\xD8', '\x0F', '\x41', '\x82', '\x8B', '\xBB', '\xFC', '\x5C',
  '\xF9', '\x0B', '\xC2', '\x1C', '\xF7', '\xA4', '\x41', '\x05', '\x84', '\x24', '\xDD', '\x05', '\x18', '\xD4', '\xC4', '\x5D',
  '\x04', '\x07', '\x33', '\x46', '\x2C', '\x1F', '\x9F', '\x2E', '\x5C', '\x88', '\x07', '\x4C', '\x8F', '\x55', '\x17', '\x9A',
  '\xFD', '\xE4', '\x62', '\xF3', '\xBF', '\xB3', '\x4C', '\x74', '\x99', '\x07', '\xED', '\xCB', '\xAD', '\xCA', '\x0E', '\x4E',
  '\xF1', '\xF8', '\xB7', '\x9C', '\x62', '\x46', '\x26', '\x2C', '\x36', '\x3B', '\xAE', '\x3B', '\x43', '\xE5', '\x59', '\x38',
  '\xCA', '\x90', '\x0E', '\x17', '\x12', '\x40', '\x53', '\x6F', '\x23', '\x37', '\x87', '\x2D', '\x2F', '\xBD', '\x41', '\xF3',
  '\x4A', '\x25', '\x4F', '\x63', '\x67', '\x47', '\x9E', '\x1E', '\x7E', '\x14', '\x01', '\xAE', '\x62', '\xEB', '\x18', '\x6B',
  '\x06', '\x4A', '\xA5', '\xE4', '\x71', '\xBA', '\x9E', '\x3F', '\xE6', '\x47', '\x2D', '\xAF', '\x91', '\xF8', '\xF1', '\x95',
  '\x57', '\x44', '\x49', '\xBE', '\x02', '\xE5', '\x49', '\x41', '\x75', '\x38', '\x35', '\xB1', '\xEF', '\x07', '\xF9', '\xC1',
  '\x09', '\xBB', '\x97', '\x40', '\xBB', '\xE2', '\x47', '\x8E', '\xD4', '\x78', '\x61', '\x79', '\x1B', '\x93', '\x80', '\xEA',
  '\x05', '\x97', '\xA6', '\xC6', '\xF2', '\x81', '\x53', '\x35', '\x7E', '\x04', '\x14', '\x45', '\x51', '\x2B', '\xE3', '\x9E',
  '\x0E', '\x81', '\x59', '\x82', '\x4E', '\x8F', '\xF4', '\x0C', '\xA7', '\xF8', '\xA9', '\x8B', '\x4E', '\x4D', '\x31', '\x23',
  '\xB8', '\x34', '\x38', '\x60', '\x9C', '\x7A', '\xBE', '\x06', '\xC7', '\x38', '\x07', '\x19', '\x24', '\x63', '\xC6', '\x3D',
  '\x40', '\x03', '\xD8', '\xE9', '\x6B', '\x99', '\x04', '\x2F', '\x2E', '\x17', '\x3F', '\xEC', '\x24', '\xD4', '\xF3', '\xC4',
  '\xEB', '\xE2', '\x00', '\x41', '\x86', '\x5C', '\xE2', '\x9F', '\x71', '\xD4', '\x58', '\xE5', '\x59', '\x45', '\xA1', '\xB8',
  '\xBC', '\xB6', '\x64', '\xF3', '\xCB', '\xCB', '\x74', '\x1E', '\x74', '\xB2', '\x47', '\x7F', '\x4C', '\x3B', '\x2C', '\x3C',
  '\xC1', '\xFF', '\xE3', '\xF3', '\x3F', '\x88', '\x4F', '\x1C', '\xAB', '\xA0', '\xD8', '\xBF', '\x01', '\xE5', '\x0A', '\x0A',
  '\x55', '\xFC', '\x1C', '\x00', '\x00'
    };
  std::string s;
  std::vector<uint8_t> v;
  std::vector<uint8_t> raw = GzDec(generate_resource, 3029);
  s.assign(raw.begin(), raw.end());
  std::string dst = subreplace(s, "%s", key);
  v.assign(dst.begin(), dst.end());
  return v;
}

static void Usage(void){
    fprintf(stderr,
        "Edge/Chrome resources.pak Disk patch tool\n"
        "lv          Compression Level\n"
        "nobak       Don't make a backup\n");
}

int doBinareFile(const char* path) {
    int lv = 9;
    bool nobak = true;
    bool ischrome = false;
    char *psec = NULL;
    const char *key = " as ";
    char impsym[2] = {0};

    if(!path || *path == 0){Usage();return 1;}

    FILE* fp=fopen(path,"rb");
    if(!fp){perror("[=] Failed to open pak");return 2;}
    fseek(fp,0,SEEK_END); long fsz=ftell(fp); fseek(fp,0,SEEK_SET);
    if(fsz<=0||fsz>512L*1024*1024){fprintf(stderr,"[!] PAK size abnormal %ld\n",fsz);fclose(fp);return 2;}
    std::vector<uint8_t> buf((size_t)fsz);
    if(fread(buf.data(),1,(size_t)fsz,fp)!=(size_t)fsz){perror("[=] Failed to read pak");fclose(fp);return 2;}
    fclose(fp);

    PakLayout L;
    if(!DetectPak(buf.data(),buf.size(),&L)){fprintf(stderr,"[!] Cannot recognize PAK header\n");return 3;}
    printf("[+] PAK: ver=%u enc=%u %s (res=%u alias=%u)\n",
           L.version,L.encoding,L.wide?"Edge Wide Head":"Chrome Narrow Head",L.count,L.alias);
    if(L.encoding!=1){fprintf(stderr,"[!] Not gzip encoded\n");return 3;}

    if (!L.wide) ischrome = true;
    // 不硬编码资源 ID，用 Edge 专属特征自动定位 PageDataService
    uint32_t ti=(uint32_t)-1, tid=0; size_t toff=0, tnext=0;
    ti = FindEdgeTargetResourceId(buf.data(), L, &tid, &toff, &tnext, ischrome);
    if(ti==(uint32_t)-1){
        fprintf(stderr,
                "Browser PageDataService resource not found\n"
                "（Feature：'%s''）\n"
                "Possible reason: The current browser version varies a lot, or the file has been modified by other tools\n",
                ischrome ? kMarkerInner : kMarkerGetPage);
        return 4;
    }
    size_t old_slot = (size_t)(tnext-toff);
    printf("[+] Automatically locate target resources id=%u, idx=%u, off=0x%zX, slot=%zu\n",
           tid, ti, toff, old_slot);

    uint8_t* slot = buf.data()+toff;
    if(old_slot<18||slot[0]!=0x1F||slot[1]!=0x8B||slot[2]!=0x08){fprintf(stderr,"[!] Illegal gzip slot\n");return 4;}

    std::vector<uint8_t> js = GzDec(slot, old_slot);
    if(js.empty()){fprintf(stderr,"[!] Failed to unzip the target resource\n");return 5;}
    printf("[+] Original JS plaintext %zu bytes\n",js.size());

    if((psec = (char *)memmem(js.data(), 13, key, 4)) == NULL){
        printf("[!] No JS import [resources id=%u]\n", tid);return 5;
    } else {
        psec += strlen(key);
        snprintf(impsym, 2, "%s", psec);
        printf("[+] JS import symbol[%s]\n", impsym);
    }

    if(memmem(js.data(), js.size(), "Portable msedge++", 17)){
        printf("[=] Hook already exists[resources id=%u], skipping to keep it idempotent\n", tid);return 0;
    }

    size_t size = js.size();
    if (ischrome) {
        uint8_t* begin = reinterpret_cast<uint8_t*>(js.data());
        std::string html(reinterpret_cast<char*>(begin), size);
        compression_html(html);
        ReplaceStringInPlace(html, R"(?hidden="${!this.showUpdateStatus_}")",
                             R"(hidden="true")");
        ReplaceStringInPlace(html, R"(?hidden="${!this.shouldShowIcons_()}")",
                         R"(hidden="true")");
        const char product_title[] =
            R"({aboutBrowserVersion}</div><div class="secondary">Powered by <a target="_blank" href="https://github.com/adonais/libportable">Portable msedge++</a> )" "1.3.0" " (by adonais)"
            R"(</div>)";
        ReplaceStringInPlace(html, R"({aboutBrowserVersion}</div>)", product_title);
        if (html.length() > size) {
          printf("[=] The source file is too big after the hook\n");
          return 6;
        }
        printf("[+] Start modifying Chrome resource files\n");
        memcpy(begin, html.c_str(), html.length());
        size = html.length();
    } else {
        std::vector<uint8_t> v2 = dec_stream(impsym);
        if(v2.empty()){fprintf(stderr,"[!] Failed to unzip stream resource\n");return 5;}
        js.insert(js.end(), std::make_move_iterator(v2.begin()), std::make_move_iterator(v2.end()));
        size = js.size();
        printf("[+] Append hook %zu bytes -> new plaintext %zu\n", v2.size(), js.size());
    }

    size_t csz=0;
    std::vector<uint8_t> cnew = GzEnc(js.data(), size, &csz, lv);
    if(cnew.empty()){fprintf(stderr,"[=] Compression failed\n");return 5;}
    printf("[+] Compressed to %zu bytes (original slot %zu, needs expansion %zd)\n",csz,old_slot,(intptr_t)csz-(intptr_t)old_slot);

    if(csz + 2 <= old_slot && (old_slot - csz - 2) <= 0xFFFF){
        // 能原地塞下：把新 gzip 流用 FEXTRA 填零撑满原槽位（不改文件长度、不动偏移表）
        // 槽位本身就是一条完整 gzip 流，长度 = old_slot，不存在额外的"长度尾"
        size_t gz_area = old_slot;
        printf("[+] Sufficient byte budget, FEXTRA in-place writeback (slot %zu, new csz %zu)\n", gz_area, csz);
        std::vector<uint8_t> ns(gz_area,0);
        memcpy(ns.data(),cnew.data(),10);          // 原样拷固定头
        ns[3]=0x04;                                // FLG: 只置 FEXTRA 位
        size_t xlen = gz_area - csz - 2;           // -2 是 XLEN 字段自身开销
        ns[10]=(uint8_t)(xlen&0xFF);
        ns[11]=(uint8_t)(xlen>>8);
        memcpy(ns.data()+12+xlen,cnew.data()+10,cnew.size()-10);  // deflate + CRC32 + ISIZE
        std::vector<uint8_t> v = GzDec(ns.data(),ns.size());
        if(v.size()!=size||memcmp(v.data(),js.data(),size)!=0){
            fprintf(stderr,"[=] Self-check failed\n");return 8;
        }
        if(!nobak){
            std::string bk=std::string(path)+".bak";
            FILE* bf=fopen(bk.c_str(),"wb");
            if(!bf){perror("[=] Backup failed");return 7;}
            fwrite(buf.data(),1,buf.size(),bf);fclose(bf);
            printf("[+] Backup: %s\n",bk.c_str());
        }
        memcpy(slot,ns.data(),gz_area);            // 恰好写满原槽位，长度不变
    } else {
        // 需要扩容：新数据写回原槽位起点，后面数据整体后移，同步更新偏移表
        // 槽位就是一条完整 gzip 流，所以新槽位长 = csz，没有额外的"长度尾"
        size_t new_slot = csz;
        ptrdiff_t delta = (ptrdiff_t)new_slot - (ptrdiff_t)old_slot;
        printf("[+] Need to expand by %td bytes, the rest of the resources will shift back\n",delta);
        if(!nobak){
            std::string bk=std::string(path)+".bak";
            FILE* bf=fopen(bk.c_str(),"wb");
            if(!bf){perror("[=] Backup failed");return 7;}
            fwrite(buf.data(),1,buf.size(),bf);fclose(bf);
            printf("[+] Backup: %s\n",bk.c_str());
        }
        size_t old_end = buf.size();
        size_t new_end = old_end + (size_t)delta;
        buf.resize(new_end, 0);
        // 把 (toff+old_slot) 到 old_end 的数据向后移 delta
        memmove(buf.data() + toff + new_slot,
                buf.data() + toff + old_slot,
                old_end - (toff + old_slot));
        // 写入新压缩 gzip 数据（末尾 CRC32/ISIZE 已在其内部）
        memcpy(buf.data() + toff, cnew.data(), csz);
        // 更新条目表：所有 offset > toff 的（包含目标资源之后的资源 和 哨兵）加 delta
        // 注意：目标资源自己的 offset 没变（toff 不动），它后面第一个条目的 offset 应该是 toff+csz
        size_t isz = L.wide?8:6;
        for(uint32_t i=0;i<=L.count;i++){
            uint8_t* p = buf.data() + L.entry_tbl_off + (size_t)i*isz;
            uint32_t* offp = (uint32_t*)(p + (L.wide?4:2));
            if(i == ti){
                // 自己：offset 保持 toff（不变）
                continue;
            }
            if(*offp > toff){
                *offp = (uint32_t)(*offp + delta);
            }
        }
        // 哨兵（i==L.count）必须等于 new_end
        uint32_t sentry_off = 0;
        EntPtr(buf.data(),L,L.count,nullptr,&sentry_off);
        if(sentry_off != new_end){
            // 显式修正哨兵
            uint8_t* sp = EntPtr(buf.data(),L,L.count,nullptr,nullptr);
            W32(sp+(L.wide?4:2),(uint32_t)new_end);
        }
    }

    // 全局自校验：重新 DetectPak 一次，确认结构合法
    PakLayout L2;
    if(!DetectPak(buf.data(),buf.size(),&L2)){
        fprintf(stderr,"[!] Modified PAK structure verification failed, not writing to disk\n");return 8;
    }
    // 再一次自动定位目标资源 id 并解压自校验
    bool vfy_ok = false;
    for(uint32_t i=0;i<L2.count;i++){
        uint32_t id,off,nid,noff;
        EntPtr(buf.data(),L2,i,&id,&off);
        EntPtr(buf.data(),L2,i+1,&nid,&noff);
        if(id==tid){
            size_t slot_sz = (size_t)noff - (size_t)off;
            if(slot_sz < 18){fprintf(stderr,"[!] The target resource slot is too small\n");return 8;}
            std::vector<uint8_t> v=GzDec(buf.data()+off, slot_sz);
            if(v.size()!=size||memcmp(v.data(),js.data(),size)!=0){
                fprintf(stderr,"[!] Checksum decompression mismatch\n");return 8;
            }
            vfy_ok = true;
            break;
        }
    }
    if(!vfy_ok){fprintf(stderr,"[!] Self-check couldn't find the target resource\n");return 8;}
    printf("[+] Self-check passed, PAK structure is valid\n");

    FILE* out=fopen(path,"wb");
    if(!out){perror("[=] Write disk failed (Edge/Chrome running?)");return 7;}
    if(fwrite(buf.data(),1,buf.size(),out)!=buf.size()){perror("[=] Failed to write pak");fclose(out);return 7;}
    fclose(out);
    printf("[+] Patch completed: %s (%zu bytes)\n",path,buf.size());
    return 0;
}
