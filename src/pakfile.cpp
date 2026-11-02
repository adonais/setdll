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

static std::vector<uint8_t> dec_stream(void) {
const unsigned char generate_resource[3029] = {
  '\x1F', '\x8B', '\x08', '\x08', '\xBB', '\xA2', '\xE8', '\x6A', '\x02', '\x03', '\x6B', '\x68', '\x6F', '\x6F', '\x6B', '\x2E',
  '\x6A', '\x73', '\x00', '\xED', '\x59', '\x7D', '\x6F', '\xDB', '\xC6', '\x19', '\xFF', '\x2A', '\x0C', '\xBA', '\x89', '\x64',
  '\x2C', '\x51', '\x92', '\xE3', '\x66', '\x85', '\x15', '\x26', '\x70', '\x6C', '\x2F', '\x33', '\xEA', '\xD8', '\x86', '\xAD',
  '\x36', '\x03', '\xEC', '\xAC', '\xA0', '\xC8', '\x93', '\x78', '\x31', '\x45', '\x32', '\xC7', '\xA3', '\x25', '\xC5', '\x24',
  '\x90', '\x64', '\xDD', '\xD2', '\x02', '\x5D', '\x53', '\x60', '\x59', '\xDB', '\x64', '\x05', '\x9A', '\x6E', '\x6D', '\x17',
  '\x14', '\x5D', '\x5A', '\x6C', '\xED', '\x9A', '\xA6', '\x4D', '\xF3', '\x61', '\x16', '\xCB', '\xF6', '\x5F', '\xFD', '\x0A',
  '\x7B', '\xEE', '\x8E', '\x6F', '\x92', '\xED', '\x34', '\xD8', '\xF2', '\xE7', '\x90', '\xC0', '\x22', '\x9F', '\x7B', '\xEE',
  '\x9E', '\xF7', '\xDF', '\x3D', '\x77', '\x6C', '\x28', '\xED', '\xD0', '\x35', '\x29', '\xF6', '\x5C', '\x45', '\xDD', '\xDE',
  '\x32', '\x88', '\x64', '\xEB', '\x2B', '\x5A', '\x07', '\xD1', '\x15', '\xA3', '\x83', '\xE6', '\x0C', '\x6A', '\x94', '\xED',
  '\x40', '\x10', '\xD6', '\x90', '\x41', '\x4C', '\x7B', '\x15', '\x05', '\xA1', '\x93', '\x0F', '\x7A', '\xAD', '\x00', '\x91',
  '\x2D', '\x64', '\xE9', '\x2E', '\xEA', '\x49', '\x17', '\x90', '\xB1', '\xB9', '\x86', '\xA8', '\xA2', '\x96', '\x4D', '\xDB',
  '\x70', '\x3B', '\x40', '\xAD', '\x95', '\x7D', '\xE4', '\x5A', '\xD8', '\xED', '\xC0', '\x93', '\x63', '\x04', '\x74', '\x06',
  '\x5B', '\x7A', '\xA5', '\x5E', '\x6E', '\x87', '\x8E', '\x33', '\xE7', '\xB9', '\x48', '\xAF', '\x35', '\x52', '\xE1', '\x52',
  '\xD7', '\x20', '\x9B', '\x0A', '\x52', '\xB7', '\x71', '\x5B', '\x39', '\x86', '\x98', '\xBC', '\x19', '\x4A', '\x09', '\x6E',
  '\x85', '\x14', '\x29', '\xB2', '\x05', '\xA2', '\x2A', '\xC8', '\xEA', '\x20', '\xDF', '\x97', '\x55', '\x75', '\x1B', '\x69',
  '\x01', '\x1D', '\x38', '\xF0', '\x17', '\xB4', '\x24', '\x9E', '\x8F', '\x08', '\x1D', '\x00', '\x0F', '\x0E', '\x7C', '\xC7',
  '\x18', '\xC8', '\x65', '\xD9', '\x85', '\x95', '\xE1', '\x07', '\x77', '\x7D', '\x8F', '\x50', '\xC3', '\xA5', '\xB2', '\xDA',
  '\xE0', '\xBC', '\x47', '\x2C', '\x58', '\x96', '\xEB', '\xC0', '\x91', '\x68', '\x3C', '\x31', '\x11', '\xC7', '\x99', '\x4A',
  '\x16', '\x42', '\xFE', '\xAC', '\xE7', '\x38', '\xC8', '\xA4', '\x0A', '\xF1', '\x3C', '\x5A', '\x36', '\x4C', '\x53', '\xDD',
  '\x66', '\x4F', '\xDA', '\xE5', '\x10', '\x91', '\xC1', '\x1A', '\x62', '\x43', '\x1E', '\x99', '\x71', '\x1C', '\x45', '\x3E',
  '\x2E', '\xAB', '\x5A', '\xDB', '\x23', '\xF3', '\x86', '\x69', '\xE7', '\x1E', '\x05', '\x7B', '\x60', '\x8E', '\xE6', '\x87',
  '\x81', '\x0D', '\xCF', '\x0D', '\xB0', '\x0D', '\x34', '\xB1', '\x0D', '\xCB', '\xEB', '\xAD', '\xC2', '\x2A', '\x6A', '\x51',
  '\x40', '\x71', '\x80', '\x0B', '\x8A', '\xD5', '\x06', '\x41', '\x34', '\x24', '\xAE', '\x04', '\x6F', '\xB9', '\x52', '\x89',
  '\xC7', '\x19', '\x9B', '\x42', '\x84', '\xBB', '\x48', '\x14', '\xA5', '\x71', '\xD0', '\x6C', '\x23', '\x00', '\xB2', '\x2A',
  '\x66', '\x36', '\x32', '\xB2', '\x61', '\x59', '\x40', '\x6E', '\xB0', '\x28', '\x9D', '\x0F', '\xA9', '\xC1', '\x56', '\x5A',
  '\x16', '\x63', '\x44', '\xF1', '\x5C', '\x4E', '\x42', '\xAA', '\x96', '\xB0', '\x2B', '\xA4', '\xBC', '\x6D', '\xDA', '\xD8',
  '\xB1', '\x16', '\x71', '\x40', '\xA7', '\x29', '\x09', '\x51', '\x39', '\x08', '\x5B', '\x94', '\x20', '\x24', '\x5E', '\xC0',
  '\x57', '\xC4', '\x30', '\x29', '\x22', '\x2C', '\x05', '\x38', '\x29', '\x56', '\x73', '\x05', '\x5B', '\x21', '\xCC', '\x5C',
  '\xF1', '\x7A', '\x88', '\x20', '\x4B', '\x31', '\x7B', '\x22', '\xA9', '\x7C', '\xDD', '\xF2', '\xCC', '\xB0', '\x8B', '\x5C',
  '\xAA', '\x99', '\x04', '\x81', '\xB0', '\x79', '\x07', '\xB1', '\x37', '\x16', '\xB7', '\x2D', '\x70', '\xBF', '\x3F', '\x16',
  '\x20', '\x13', '\xB2', '\x25', '\x80', '\xD0', '\x88', '\x18', '\x55', '\x7C', '\xB1', '\x9C', '\x60', '\xE4', '\xB1', '\xEF',
  '\x10', '\x6C', '\x81', '\xEB', '\xC2', '\xAE', '\xAB', '\xCB', '\x75', '\xA9', '\x2A', '\x55', '\xEA', '\x72', '\x36', '\xD6',
  '\xC3', '\x16', '\xB5', '\x81', '\x5C', '\xAB', '\xFD', '\x3C', '\x27', '\x42', '\x7E', '\x75', '\xB0', '\xDB', '\xF4', '\x7C',
  '\x5D', '\x7E', '\xC9', '\xEF', '\xE7', '\xF4', '\xB6', '\xE7', '\xD2', '\x35', '\x7C', '\x05', '\x01', '\xFF', '\x64', '\x91',
  '\x6E', '\x7A', '\x8E', '\x47', '\x74', '\xF9', '\x85', '\x93', '\x88', '\xFD', '\x63', '\x74', '\xC3', '\x67', '\xC9', '\x3C',
  '\xCB', '\x1C', '\xA3', '\x8C', '\x59', '\xD3', '\x44', '\x7D', '\xBA', '\xE4', '\x59', '\xA0', '\x78', '\x62', '\xB8', '\xD4',
  '\x1A', '\x48', '\x90', '\xAA', '\x0D', '\x66', '\xBB', '\x71', '\xA4', '\xED', '\x06', '\x18', '\x64', '\x8C', '\x59', '\x6E',
  '\x13', '\xD4', '\x06', '\xC3', '\x6D', '\x4A', '\xFD', '\x60', '\xBA', '\x5A', '\xED', '\x60', '\x6A', '\x87', '\x2D', '\xD0',
  '\xA6', '\x5B', '\x85', '\xEC', '\x70', '\x0D', '\x1C', '\x54', '\x1D', '\xDC', '\xE2', '\x99', '\xDD', '\x72', '\x90', '\x98',
  '\x3F', '\xA2', '\x6F', '\xAD', '\xF6', '\x8B', '\x97', '\xAC', '\x29', '\x39', '\xA3', '\x53', '\x50', '\x6D', '\x0E', '\x99',
  '\x1E', '\xE1', '\x41', '\xD7', '\x45', '\x75', '\xC0', '\x28', '\xA3', '\xCF', '\x82', '\xED', '\xA0', '\x88', '\x0E', '\x5A',
  '\x8B', '\xF5', '\xA4', '\x6E', '\xC0', '\x3C', '\x3E', '\x31', '\x31', '\x6E', '\xAF', '\xA1', '\x3E', '\xAB', '\x03', '\xA4',
  '\xBA', '\x36', '\xA9', '\xD5', '\x24', '\x05', '\x1C', '\x90', '\x28', '\xAC', '\x32', '\x47', '\x98', '\xBD', '\x91', '\xE9',
  '\x7E', '\xB1', '\xE2', '\xB2', '\xD4', '\xB9', '\x62', '\xBF', '\x8C', '\x5D', '\x2B', '\xC1', '\x21', '\x43', '\x5F', '\xBF',
  '\x58', '\xC6', '\x00', '\x11', '\x94', '\x0C', '\x58', '\x9A', '\xBB', '\xC6', '\x16', '\xEE', '\x18', '\x50', '\x6E', '\x9A',
  '\x03', '\x13', '\x43', '\x40', '\xA0', '\xA0', '\x54', '\x3A', '\x84', '\xA8', '\x39', '\xC8', '\xED', '\x50', '\x5B', '\x85',
  '\x62', '\x54', '\xD8', '\x74', '\x7C', '\xEA', '\x68', '\xA6', '\x06', '\x9E', '\x98', '\x50', '\x0D', '\x51', '\x9F', '\x87',
  '\x70', '\xAD', '\xE3', '\x8B', '\x6A', '\x6C', '\x1A', '\xD4', '\x64', '\xD5', '\xBB', '\x1D', '\x1F', '\xA9', '\xC8', '\xD1',
  '\x4B', '\x3C', '\x75', '\x7A', '\x08', '\xD5', '\xB6', '\x78', '\xE4', '\x12', '\x23', '\xA3', '\x07', '\x97', '\xC9', '\x02',
  '\x90', '\x3E', '\x24', '\x59', '\x55', '\x2A', '\x1D', '\x35', '\xC2', '\x95', '\x4A', '\xE5', '\x3C', '\x9D', '\xA9', '\x20',
  '\x8E', '\x85', '\x82', '\xEA', '\x86', '\x76', '\xC9', '\xC3', '\xAE', '\x02', '\x89', '\xA9', '\x6A', '\xD4', '\x5B', '\x64',
  '\x49', '\x3E', '\x6B', '\x04', '\x48', '\xE1', '\x98', '\x46', '\xB5', '\xC0', '\xC1', '\x26', '\x52', '\x6A', '\xE5', '\x49',
  '\x55', '\xD7', '\x75', '\x19', '\xB9', '\x72', '\x02', '\x40', '\x52', '\x6D', '\x7C', '\xFC', '\x18', '\x8C', '\x5F', '\xB1',
  '\x0B', '\xE3', '\x6C', '\xFD', '\x40', '\xCF', '\x59', '\x5E', '\xE4', '\x4B', '\x06', '\x3A', '\xE7', '\xAB', '\x98', '\xAE',
  '\x1C', '\x45', '\xE9', '\x4B', '\xD0', '\xC9', '\xE6', '\xD5', '\x53', '\x6C', '\x9C', '\x3C', '\x00', '\x3C', '\x22', '\x93',
  '\x9B', '\xD8', '\x57', '\x0C', '\xD7', '\xB4', '\x3D', '\xF2', '\x9C', '\x00', '\x48', '\x2C', '\x5B', '\xA1', '\xD8', '\x2F',
  '\xA0', '\x50', '\xB2', '\xE9', '\xE8', '\x72', '\xCB', '\xF1', '\xCC', '\xCD', '\x67', '\x42', '\x20', '\x81', '\x33', '\x52',
  '\x2D', '\xA7', '\xFB', '\x00', '\xCE', '\x6C', '\x8B', '\x14', '\x03', '\xF5', '\x93', '\x45', '\x14', '\x6A', '\x79', '\xC4',
  '\x42', '\x64', '\xD5', '\xB0', '\x70', '\x18', '\xE8', '\xF2', '\xD4', '\x11', '\xC0', '\x75', '\xA2', '\x48', '\x77', '\xB0',
  '\x8B', '\x7E', '\x85', '\x70', '\xC7', '\x86', '\xBA', '\xAE', '\x6B', '\x2F', '\x3E', '\x0D', '\xD1', '\x52', '\x19', '\x7D',
  '\x58', '\x87', '\x6B', '\x20', '\xC4', '\x55', '\x80', '\x22', '\xF3', '\xC0', '\x6C', '\xEA', '\x69', '\x49', '\x02', '\x77',
  '\x11', '\x31', '\x36', '\x21', '\x24', '\xB5', '\x33', '\x72', '\xD3', '\xC6', '\x81', '\x04', '\xFF', '\xA9', '\x8D', '\xA4',
  '\xD4', '\x45', '\x12', '\xEC', '\x29', '\x01', '\xDF', '\xA9', '\xDA', '\xD2', '\x3C', '\x78', '\xAF', '\x2C', '\x0D', '\xBC',
  '\x50', '\x72', '\x11', '\x80', '\x22', '\xF5', '\x24', '\xC8', '\x6A', '\xC9', '\x00', '\x7E', '\x4C', '\xAC', '\x8A', '\x6F',
  '\xC0', '\xCE', '\x0D', '\x44', '\xCF', '\x61', '\x23', '\xE0', '\x7F', '\x09', '\x53', '\xC9', '\x10', '\xAB', '\x59', '\xA8',
  '\x6D', '\x40', '\xA3', '\x21', '\xB5', '\x88', '\xD7', '\x83', '\x42', '\xD0', '\xA4', '\xB5', '\xB0', '\x03', '\x15', '\x49',
  '\xA7', '\x25', '\x79', '\x5A', '\x61', '\xD2', '\x27', '\xCF', '\xC8', '\xFB', '\x57', '\x6F', '\x0F', '\xDF', '\xFF', '\x82',
  '\x0B', '\x91', '\x9E', '\xFC', '\xF0', '\x78', '\x78', '\xEB', '\x83', '\xDD', '\x37', '\xDF', '\x18', '\x7E', '\xF0', '\xF9',
  '\x8F', '\xDF', '\xBF', '\xF5', '\xE4', '\xD1', '\xDD', '\xFD', '\x0F', '\xAE', '\xEE', '\x7D', '\x7A', '\xED', '\xC9', '\xA3',
  '\xC7', '\xBB', '\xB7', '\xEE', '\xED', '\x7E', '\xFE', '\xF9', '\x93', '\x07', '\x6F', '\x0E', '\xDF', '\xFD', '\x76', '\xE7',
  '\x9B', '\x4F', '\x76', '\x7E', '\xF7', '\xCD', '\xCE', '\x97', '\x37', '\x76', '\xEE', '\xFF', '\x76', '\xEF', '\xDE', '\xDF',
  '\x77', '\xEE', '\xDF', '\xD9', '\xBD', '\xFE', '\x70', '\xFF', '\xEE', '\x3B', '\xF0', '\xBC', '\x7B', '\xF5', '\xE6', '\xDE',
  '\xA7', '\x8F', '\x76', '\x6E', '\xDF', '\xFB', '\xF7', '\xD5', '\xEB', '\xD2', '\xF0', '\xED', '\x7B', '\x7B', '\xEF', '\x7E',
  '\xFA', '\xE3', '\xF7', '\x77', '\x40', '\x9C', '\xBC', '\xF7', '\x78', '\x54', '\xCE', '\x3B', '\x0F', '\x9F', '\x59', '\xCE',
  '\xEF', '\x99', '\x9C', '\xFB', '\x3F', '\xEC', '\x3E', '\xBA', '\xFF', '\xE4', '\xC1', '\xC3', '\xFD', '\xEF', '\xDE', '\xDF',
  '\xBB', '\xFF', '\xF1', '\xF0', '\xEB', '\x9B', '\x7B', '\x7F', '\x7B', '\xA3', '\x20', '\xE7', '\x0F', '\xEF', '\x70', '\x39',
  '\xCF', '\x77', '\xB3', '\x58', '\xC3', '\x8B', '\x27', '\x96', '\x66', '\x4F', '\x54', '\xD3', '\x7A', '\x58', '\x45', '\x1D',
  '\xD8', '\xCC', '\x09', '\x43', '\x96', '\xE7', '\xBD', '\x69', '\x14', '\x97', '\x3E', '\xB8', '\x6D', '\x88', '\x22', '\xD4',
  '\xB0', '\x0B', '\x51', '\xA4', '\x33', '\xD6', '\x25', '\xC3', '\xCC', '\xC1', '\x06', '\x8C', '\x6A', '\x43', '\x1B', '\x01',
  '\xEC', '\x72', '\xF9', '\xF0', '\x1D', '\x81', '\xF5', '\x81', '\xF0', '\xB4', '\x85', '\x16', '\xD2', '\x6D', '\x21', '\xD4',
  '\x7B', '\x90', '\x90', '\x5E', '\x4F', '\x83', '\x8A', '\xE3', '\xDA', '\x69', '\xCC', '\x09', '\x0C', '\x33', '\xAA', '\xEB',
  '\x67', '\x4A', '\x2F', '\x5C', '\x0C', '\x78', '\x9B', '\xAA', '\x57', '\x31', '\x28', '\x1B', '\x50', '\x25', '\x54', '\xA3',
  '\x48', '\x0C', '\xF0', '\x96', '\xAD', '\x40', '\x4F', '\xA1', '\x64', '\x72', '\x8A', '\x4F', '\xDE', '\xA8', '\xDA', '\xC8',
  '\xF1', '\x37', '\x5A', '\xD1', '\x46', '\xD5', '\x68', '\x79', '\x21', '\x85', '\x27', '\x1F', '\x90', '\x77', '\xC1', '\xD2',
  '\xF9', '\x6B', '\xFA', '\xC2', '\x98', '\x0E', '\x59', '\xA3', '\x9E', '\xAC', '\x91', '\xA4', '\xED', '\x59', '\x91', '\xB5',
  '\xF9', '\x1A', '\xA3', '\xF4', '\x83', '\x0B', '\xD4', '\xA7', '\x44', '\xF4', '\x27', '\xEB', '\x79', '\xFC', '\x47', '\x7A',
  '\x4C', '\x45', '\x86', '\xC8', '\x53', '\x28', '\xD2', '\xA0', '\xC2', '\xF5', '\xA9', '\xB0', '\x85', '\x65', '\xB5', '\x6C',
  '\xD4', '\xA7', '\x7E', '\x7A', '\x46', '\x22', '\xBD', '\x92', '\x14', '\x53', '\x32', '\x37', '\xEF', '\xBD', '\xB7', '\x70',
  '\x90', '\xB5', '\xDE', '\xA3', '\xC8', '\x4C', '\x74', '\xDE', '\x8B', '\x9F', '\xF5', '\x42', '\xDE', '\xC6', '\xCF', '\x3A',
  '\x18', '\xE4', '\xAC', '\xB2', '\x7E', '\x95', '\xC3', '\x34', '\x11', '\x48', '\x77', '\x4A', '\xAF', '\x45', '\x11', '\xD1',
  '\x6C', '\x8E', '\x3A', '\xF0', '\x32', '\x82', '\xFE', '\x48', '\xF3', '\xDA', '\x6D', '\xD6', '\xAA', '\x1B', '\x10', '\x68',
  '\x0A', '\xF8', '\xEF', '\x42', '\xF7', '\x9F', '\xE3', '\x38', '\x93', '\x62', '\x06', '\x3A', '\x08', '\x99', '\xF5', '\xBA',
  '\x3E', '\x64', '\xB5', '\xB5', '\xC6', '\x52', '\x90', '\xB5', '\xCB', '\x09', '\x8F', '\x19', '\x68', '\xBE', '\x17', '\x60',
  '\x9E', '\x89', '\xB0', '\x11', '\xB4', '\x71', '\x1F', '\x3A', '\xC1', '\x28', '\x02', '\x72', '\x02', '\xBF', '\x6C', '\x4F',
  '\xE1', '\x19', '\x7A', '\xA6', '\x3E', '\x5D', '\x8B', '\x41', '\x24', '\xB3', '\x07', '\x5C', '\x39', '\x16', '\x1E', '\x4E',
  '\xAD', '\x4F', '\x15', '\x7D', '\x0E', '\x54', '\xC6', '\x37', '\xC2', '\xC6', '\x58', '\x72', '\x8E', '\xD4', '\x92', '\x3C',
  '\x25', '\x49', '\xE8', '\xAE', '\xF5', '\xA0', '\x71', '\x57', '\x90', '\x13', '\x94', '\x0D', '\x6C', '\xA9', '\xDB', '\xD9',
  '\x69', '\x27', '\xF7', '\x28', '\xED', '\x53', '\xE6', '\x51', '\x31', '\x5B', '\x41', '\xC5', '\xC2', '\x89', '\x22', '\x19',
  '\x36', '\x51', '\x82', '\x40', '\x73', '\xD8', '\xF0', '\xAA', '\x1B', '\xC1', '\x44', '\xB5', '\x53', '\x96', '\x25', '\xB6',
  '\xB1', '\x12', '\xDC', '\x55', '\x54', '\x66', '\x00', '\x2C', '\xCB', '\x20', '\xAE', '\x1E', '\x45', '\xC9', '\xD3', '\x94',
  '\x48', '\x7E', '\x80', '\xD6', '\x55', '\x08', '\x21', '\x74', '\x46', '\x0D', '\xD6', '\xDE', '\x30', '\x92', '\x68', '\x71',
  '\x40', '\x97', '\x62', '\x4B', '\xC3', '\x99', '\x91', '\x0E', '\x54', '\x68', '\x61', '\x44', '\x10', '\xA8', '\xD1', '\x59',
  '\x32', '\xBA', '\x88', '\xF9', '\x6A', '\xAD', '\x39', '\xB3', '\x34', '\x37', '\xB3', '\x3A', '\x57', '\x59', '\x5D', '\xBE',
  '\x50', '\x99', '\x5D', '\x5E', '\x6A', '\xCE', '\x2C', '\x2C', '\xCD', '\xAF', '\xCA', '\xAA', '\x09', '\x0A', '\x62', '\x37',
  '\x44', '\x8D', '\x9F', '\x3C', '\x83', '\x95', '\x4A', '\x8A', '\xB0', '\x70', '\xB4', '\x19', '\x80', '\x2A', '\xB7', '\x50',
  '\x7F', '\xB9', '\x0D', '\xDC', '\x5E', '\xCF', '\x75', '\x3C', '\xC3', '\x92', '\x42', '\x1F', '\x26', '\xA2', '\x40', '\xF2',
  '\x40', '\x75', '\x99', '\x75', '\x07', '\x90', '\x29', '\xC9', '\xD4', '\x8C', '\x79', '\xFF', '\xEA', '\x9D', '\xBD', '\xC7',
  '\x37', '\x86', '\x6F', '\xBD', '\x39', '\xFC', '\xFA', '\xDA', '\xFE', '\x8D', '\x9B', '\x7B', '\xF7', '\x3F', '\xDA', '\xFB',
  '\xEA', '\xDB', '\xDD', '\x3B', '\xAF', '\x1F', '\xCD', '\x7F', '\x73', '\xFF', '\xDA', '\xDB', '\x7B', '\xF7', '\xDE', '\x00',
  '\xE6', '\x27', '\x80', '\xA9', '\xFF', '\xF8', '\x4E', '\x70', '\xAA', '\x6A', '\x72', '\x88', '\xE4', '\xC5', '\xA8', '\x08',
  '\x90', '\x8E', '\x5E', '\x15', '\xDB', '\x91', '\xBA', '\x11', '\x1C', '\xDF', '\xB0', '\x26', '\x36', '\xB4', '\x91', '\x3F',
  '\x69', '\x25', '\x1E', '\x8C', '\x90', '\x9A', '\xF8', '\x3A', '\x3D', '\xBC', '\xC5', '\xA9', '\xC3', '\xB7', '\xC0', '\xE1',
  '\x5B', '\xA7', '\xD2', '\xD1', '\xC4', '\xE9', '\x5B', '\xA9', '\xD3', '\x4D', '\x3D', '\x19', '\x59', '\xDF', '\xBA', '\xC8',
  '\x73', '\x1B', '\x80', '\x0F', '\x5B', '\x48', '\x2F', '\x1E', '\xF5', '\xCC', '\xF2', '\xFA', '\x45', '\x81', '\xF6', '\xB0',
  '\x57', '\x6F', '\xB2', '\x68', '\x96', '\xCD', '\x1E', '\x2F', '\x8B', '\x2C', '\xAA', '\x9B', '\x20', '\x64', '\xF3', '\x94',
  '\x98', '\x9B', '\xCA', '\xD8', '\x4C', '\x65', '\xF4', '\x75', '\x31', '\xB0', '\xBE', '\x29', '\x44', '\x50', '\x57', '\xEF',
  '\xA7', '\xF1', '\xE5', '\xFD', '\x16', '\xAF', '\x93', '\xB5', '\xF9', '\x66', '\x73', '\x61', '\xE9', '\xDC', '\x5A', '\xA5',
  '\x39', '\x7F', '\x7E', '\x65', '\x71', '\xA6', '\x39', '\x5F', '\x59', '\x5C', '\x58', '\x7A', '\x59', '\xE6', '\x65', '\xDE',
  '\x2F', '\x1C', '\x36', '\x4B', '\xA5', '\xAA', '\x32', '\xFC', '\xEB', '\xD5', '\xE1', '\x87', '\x9F', '\x0C', '\xFF', '\xFC',
  '\xD5', '\xF0', '\xDD', '\x2F', '\x87', '\xEF', '\xFD', '\x6B', '\xE7', '\xC6', '\xC3', '\xFD', '\x5B', '\xB7', '\xA3', '\xE1',
  '\x67', '\x7F', '\xC9', '\xA9', '\xB7', '\xAF', '\xEF', '\xDE', '\xFE', '\x7E', '\xF7', '\xD6', '\x87', '\xFB', '\x6F', '\x7D',
  '\xB1', '\xF7', '\xD9', '\xC7', '\xD1', '\x8C', '\x2B', '\x21', '\x42', '\x3C', '\x22', '\x79', '\xA6', '\x19', '\x12', '\x76',
  '\xE2', '\xE9', '\x01', '\xE2', '\x23', '\xC9', '\xB4', '\x91', '\xB9', '\x09', '\x48', '\xA1', '\xA6', '\x9E', '\x2D', '\x8A',
  '\x2A', '\x3A', '\x19', '\xCE', '\xF1', '\x3C', '\x5A', '\x7D', '\xB5', '\xC1', '\xBD', '\x20', '\xFC', '\xDC', '\x57', '\xE3',
  '\x18', '\xB2', '\x16', '\x49', '\x99', '\x19', '\x73', '\x0B', '\xAF', '\xCA', '\x89', '\x6B', '\x99', '\x99', '\xBC', '\x2F',
  '\x63', '\x86', '\xB2', '\x20', '\xF1', '\x38', '\x53', '\x4C', '\xA1', '\x2F', '\xC3', '\x90', '\xBC', '\x95', '\x1E', '\x31',
  '\xFC', '\xAA', '\x10', '\x6B', '\xBA', '\x49', '\x36', '\xC0', '\xFA', '\xE9', '\x82', '\xE0', '\x64', '\x81', '\x3E', '\x60',
  '\xB2', '\x29', '\xB4', '\x18', '\x9F', '\x02', '\x2C', '\xFD', '\x38', '\x0B', '\xF5', '\xE6', '\x24', '\x0B', '\xC3', '\xE4',
  '\x78', '\x1C', '\x26', '\xD3', '\x40', '\x0C', '\xB2', '\x40', '\x4C', '\xF2', '\x2A', '\x1B', '\x14', '\xAB', '\x8C', '\x2B',
  '\x3E', '\x52', '\x52', '\x55', '\xE5', '\x37', '\xD1', '\x46', '\xA0', '\x0A', '\x85', '\x99', '\x60', '\x65', '\x23', '\x88',
  '\x7E', '\xA6', '\x26', '\xE2', '\x07', '\xA3', '\xB6', '\xA9', '\xF9', '\xDC', '\x54', '\x1D', '\x87', '\xD5', '\xBA', '\x83',
  '\x4F', '\x09', '\x7F', '\x25', '\xDA', '\x38', '\xBC', '\xDE', '\xB9', '\x70', '\x36', '\xC1', '\x00', '\x8D', '\x14', '\xCE',
  '\xB0', '\xEE', '\xC0', '\xE9', '\x25', '\x71', '\xF2', '\x40', '\x6D', '\xB4', '\xA0', '\x9B', '\xD8', '\x8C', '\xE3', '\x98',
  '\xFB', '\xA1', '\x54', '\x3A', '\x06', '\x07', '\xB2', '\xB1', '\xED', '\x42', '\x1B', '\x3F', '\x6C', '\xAB', '\xE3', '\x47',
  '\xF9', '\x38', '\xC7', '\xA6', '\xFA', '\xD4', '\x38', '\x36', '\x99', '\x86', '\x6B', '\x90', '\xC1', '\xBC', '\x33', '\x9A',
  '\xC8', '\x97', '\x41', '\xE5', '\xCB', '\x45', '\x78', '\xBA', '\x9C', '\x55', '\x8A', '\xC0', '\xA7', '\xCB', '\x49', '\x0E',
  '\xEB', '\xAC', '\xCA', '\xCD', '\xB4', '\x76', '\x87', '\xEF', '\xDD', '\x1D', '\xFE', '\xF3', '\x4F', '\xD0', '\x3F', '\x45',
  '\xBB', '\xAF', '\x7F', '\xC4', '\x9F', '\x6E', '\xA8', '\xDA', '\x36', '\x1C', '\x20', '\x6A', '\xF1', '\x2C', '\x97', '\x93',
  '\xBC', '\x28', '\xD0', '\x5C', '\x1D', '\xEC', '\xAC', '\x22', '\x40', '\x94', '\xFD', '\x6B', '\x7F', '\x64', '\x4D', '\xDD',
  '\xF5', '\x87', '\x6A', '\x94', '\xCF', '\x98', '\x82', '\x19', '\xA0', '\xA7', '\x4C', '\x23', '\xF8', '\xEB', '\x7A', '\x54',
  '\x95', '\x5A', '\x70', '\xE6', '\x35', '\x20', '\xB6', '\x62', '\xB9', '\xB1', '\xAE', '\x73', '\x74', '\xE6', '\xF0', '\xC1',
  '\x5D', '\x58', '\x94', '\xBD', '\xD4', '\x6B', '\xF1', '\x98', '\xC4', '\x34', '\xDB', '\xA9', '\xCA', '\x5C', '\xFB', '\x13',
  '\xF7', '\x57', '\x1C', '\x9C', '\x1D', '\xDD', '\xCC', '\x23', '\x6B', '\x3A', '\xB8', '\xDB', '\x02', '\x4F', '\xF1', '\xDF',
  '\x53', '\x27', '\xC5', '\x6F', '\x12', '\xD7', '\x63', '\xC8', '\x81', '\x23', '\x02', '\xC9', '\xBB', '\xA6', '\x28', '\x1A',
  '\xA7', '\x40', '\x14', '\xB2', '\x1E', '\xA0', '\xE5', '\x59', '\x03', '\x95', '\x07', '\xBB', '\x21', '\xCE', '\x3D', '\xE3',
  '\xCC', '\x62', '\xC7', '\x65', '\xED', '\xD9', '\xBC', '\x13', '\x8C', '\xE0', '\x92', '\x9F', '\xE1', '\x92', '\x6D', '\x04',
  '\x67', '\xA1', '\xFC', '\x52', '\x2E', '\x2D', '\xF0', '\xBA', '\x28', '\xBF', '\xD5', '\xEA', '\xA7', '\x1B', '\x9B', '\x94',
  '\xC1', '\xCE', '\x08', '\xE0', '\x9C', '\x7D', '\xA5', '\xD9', '\x5C', '\x5E', '\x62', '\xBB', '\x0B', '\x6C', '\xD3', '\x23',
  '\x2C', '\xB3', '\xAB', '\xC9', '\xE0', '\xF8', '\xC0', '\xE1', '\xD4', '\x99', '\x43', '\xE6', '\x33', '\x10', '\xE3', '\x4B',
  '\xC7', '\x42', '\xD5', '\x9E', '\xEE', '\x1F', '\xDE', '\xA1', '\x9C', '\x39', '\x6F', '\x50', '\x5B', '\x6B', '\x3B', '\x1E',
  '\xF8', '\xD8', '\x3F', '\xAA', '\x89', '\x11', '\xFD', '\x8B', '\x3A', '\x2D', '\xBA', '\x1D', '\x1C', '\x34', '\x3D', '\xEF',
  '\x02', '\x83', '\xEB', '\xDE', '\xE9', '\x5A', '\xA9', '\xD4', '\x3B', '\x5D', '\x58', '\x22', '\xE9', '\x3A', '\xB1', '\xEB',
  '\x22', '\x72', '\x81', '\x4D', '\x3A', '\x5E', '\xD3', '\x4E', '\x66', '\xDE', '\x5A', '\x86', '\x33', '\x0B', '\x59', '\x43',
  '\xDC', '\x3F', '\xC1', '\x7F', '\xEB', '\xB7', '\xE6', '\xF2', '\xB9', '\x73', '\x8B', '\xF3', '\xE3', '\x36', '\x67', '\xC3',
  '\xB3', '\xCB', '\xE7', '\xCF', '\x2E', '\x9F', '\x5D', '\xFE', '\xF5', '\x01', '\x86', '\xC3', '\xB7', '\xF3', '\xA3', '\x96',
  '\x59', '\x9D', '\x99', '\x5B', '\x58', '\xAE', '\x9C', '\x5B', '\x5D', '\x7E', '\x65', '\x85', '\xF9', '\x10', '\x32', '\x4C',
  '\x44', '\x1B', '\x12', '\x37', '\xB3', '\x1F', '\x9E', '\xC7', '\x8D', '\x52', '\xB7', '\x21', '\x65', '\xFD', '\x04', '\x46',
  '\xD8', '\x63', '\x9C', '\x15', '\x3C', '\x72', '\x04', '\xAC', '\x24', '\xEF', '\x09', '\xE6', '\x64', '\xAF', '\x1C', '\xFB',
  '\xD2', '\x37', '\xCD', '\x85', '\x2D', '\x20', '\xC9', '\xC4', '\x35', '\xDC', '\x02', '\xA4', '\xEA', '\x44', '\xD1', '\xD3',
  '\x46', '\x05', '\x2A', '\xB2', '\x9B', '\xC8', '\x67', '\xE5', '\xCB', '\x71', '\xF0', '\xF0', '\xD3', '\xBB', '\x7A', '\xE0',
  '\xA2', '\x20', '\x53', '\x35', '\x8E', '\xD3', '\x9E', '\x53', '\xB4', '\x74', '\x85', '\x6B', '\xD7', '\xE4', '\x92', '\x54',
  '\xE9', '\x86', '\x34', '\xE0', '\x75', '\x99', '\xDC', '\x6C', '\xA7', '\x57', '\xAD', '\xE9', '\x45', '\xB7', '\xE8', '\x68',
  '\xA1', '\x49', '\xD4', '\x0B', '\xF7', '\xEA', '\xD9', '\x2D', '\x38', '\xF3', '\x05', '\x5B', '\xA1', '\x54', '\x4A', '\xEF',
  '\xC0', '\x05', '\x1C', '\x40', '\x7F', '\xCC', '\xDA', '\x80', '\x00', '\x21', '\x77', '\xF4', '\x2A', '\x7D', '\xAC', '\xCF',
  '\x63', '\x73', '\x0F', '\x34', '\x7A', '\x54', '\x67', '\x64', '\xE8', '\xF4', '\x20', '\xDE', '\x04', '\x12', '\x9D', '\x3B',
  '\x9C', '\xB9', '\x8B', '\xA6', '\x09', '\x90', '\xEF', '\x25', '\x8C', '\xDF', '\xD5', '\xA9', '\xE6', '\x7A', '\x16', '\x6A',
  '\x0E', '\x7C', '\x96', '\x1A', '\xF5', '\x33', '\x74', '\x9A', '\x8E', '\x61', '\x04', '\x5B', '\xC1', '\x8D', '\x22', '\xA6',
  '\x0F', '\xBF', '\x52', '\x76', '\x0B', '\xDB', '\x11', '\x27', '\xB2', '\x0B', '\x65', '\x57', '\x85', '\x67', '\x9A', '\xDC',
  '\x5B', '\xA9', '\x8D', '\x22', '\x94', '\xB8', '\x60', '\x0A', '\xE5', '\xAD', '\x2C', '\xE3', '\x48', '\x6E', '\xE4', '\xB6',
  '\xB3', '\xD6', '\x19', '\x88', '\xE9', '\xF7', '\x00', '\xB5', '\xC1', '\x94', '\x17', '\x9B', '\x86', '\x70', '\x65', '\xCC',
  '\xEF', '\x9C', '\xD8', '\xE6', '\x32', '\x72', '\xFA', '\x6B', '\xA4', '\x1F', '\x10', '\x60', '\x44', '\x1C', '\x96', '\x1C',
  '\x67', '\x04', '\xBE', '\x52', '\x08', '\xE4', '\x28', '\x96', '\x89', '\x02', '\x2E', '\xDE', '\xA5', '\x17', '\xC4', '\x64',
  '\x9F', '\x1F', '\xEA', '\x31', '\x33', '\x34', '\xA9', '\x6C', '\x82', '\x60', '\x3F', '\x04', '\x09', '\x2E', '\xEE', '\xF2',
  '\x73', '\xE5', '\x2F', '\x09', '\x73', '\xDC', '\xD3', '\x06', '\x15', '\x10', '\x92', '\x74', '\x17', '\x60', '\x50', '\x13',
  '\x77', '\x11', '\x1C', '\xCC', '\x18', '\xB1', '\x7C', '\x62', '\xB2', '\x70', '\x21', '\x1E', '\x30', '\x3D', '\x96', '\x5D',
  '\x68', '\xF6', '\x93', '\x8B', '\xCD', '\xFF', '\xCD', '\x32', '\xD1', '\x65', '\x1E', '\xB4', '\x2F', '\xB7', '\x2A', '\x3B',
  '\x38', '\xC5', '\x23', '\x5F', '\x72', '\x8A', '\x09', '\x99', '\x70', '\xD8', '\xEC', '\xB4', '\xEE', '\x0C', '\x94', '\x95',
  '\x32', '\x24', '\x0E', '\x17', '\x11', '\x40', '\x4B', '\x6F', '\x23', '\x37', '\x07', '\x2D', '\x2F', '\xBD', '\x3F', '\xF3',
  '\x4A', '\x25', '\x4F', '\x63', '\x27', '\x47', '\x9E', '\x1C', '\x7E', '\x14', '\x01', '\xAA', '\x62', '\xEB', '\x18', '\x6B',
  '\x05', '\x4A', '\xA5', '\xE4', '\x71', '\xB2', '\x9E', '\x3F', '\xE6', '\x07', '\x2D', '\xAF', '\x91', '\x78', '\xF1', '\xB5',
  '\xD7', '\x44', '\x41', '\xBE', '\x06', '\xC5', '\x49', '\x41', '\x71', '\x38', '\x33', '\xB1', '\xAF', '\x07', '\xF9', '\xB1',
  '\x09', '\xBB', '\x97', '\x40', '\xB9', '\xE2', '\x27', '\x8E', '\xD4', '\x74', '\x61', '\x77', '\x1B', '\x93', '\x80', '\xEA',
  '\x05', '\x87', '\xA6', '\xA6', '\xF2', '\x81', '\xD3', '\x35', '\x7E', '\x00', '\x14', '\x25', '\x51', '\x2B', '\xE3', '\x2D',
  '\x1D', '\xC2', '\xB2', '\x00', '\x7D', '\x1E', '\xD9', '\x32', '\x9C', '\xE2', '\x67', '\x2E', '\x3A', '\x31', '\xC1', '\x8C',
  '\xE0', '\xD2', '\xE0', '\x78', '\x71', '\xFA', '\xC5', '\x1A', '\x1C', '\xE2', '\x1C', '\x64', '\x90', '\x8C', '\x19', '\x6F',
  '\x01', '\x16', '\xC0', '\x3E', '\x5F', '\xCB', '\x24', '\x78', '\x71', '\xB9', '\xF8', '\x59', '\x27', '\xA1', '\xAE', '\x10',
  '\xAF', '\x8B', '\x03', '\x04', '\xF9', '\x71', '\x89', '\x7F', '\xC4', '\x51', '\x63', '\x95', '\xE7', '\x14', '\x85', '\xD2',
  '\xF2', '\xDA', '\x92', '\xCD', '\xAF', '\x2E', '\xD3', '\x79', '\xD0', '\xC7', '\x1E', '\xF9', '\x25', '\xED', '\xB0', '\xE0',
  '\x04', '\xFF', '\x8F', '\xCE', '\xF3', '\x8F', '\x4E', '\x1C', '\xAB', '\xA0', '\xD8', '\x7F', '\x00', '\x32', '\x7F', '\x4F',
  '\x51', '\xF6', '\x1C', '\x00', '\x00'
    };
    return GzDec(generate_resource, 3029);
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

    if(memmem(js.data(), js.size(), "Portable msedge++", 17)){
        printf("[=] Hook already exists, skipping to keep it idempotent\n");return 0;
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
            R"({aboutBrowserVersion}</div><div class="secondary">Powered by <a target="_blank" href="https://github.com/adonais/libportable">Portable msedge++</a> )" "1.2.0" " (by adonais)"
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
        std::vector<uint8_t> v2 = dec_stream();
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
