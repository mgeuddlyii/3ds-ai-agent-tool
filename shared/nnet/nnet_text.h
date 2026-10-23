/*
 * nnet_text.h — BPE 词表（3DS 端）
 *
 * 词表由 tools/guia/export_vocab.py 从训练端的 bpe.pkl 导出成
 * app/data/guide_vocab.txt（GVB1 纯文本），构建时由 tools/build.py
 * 经 bin2s **内嵌进 .cia**。加载后常驻内存，不再碰文件系统。
 *
 * 为什么内嵌而不是放 SD 卡：词表是检索层的必需输入，而 BPE 编码在
 * 词表没加载时直接返回 -1 —— 整个检索会静默变成"命中 0 条"。
 * 一个只表现为"搜不到"、屏上又看不出异常的缺失依赖，比崩溃难查得多。
 * SD 卡拔了也要能用，这本来就符合"全离线"的定位。
 */
#ifndef NNET_TEXT_H
#define NNET_TEXT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t off;      /* 在 heap 里的字节偏移 */
    uint8_t  len;      /* 字节长度（<256，语料里最长 144 字节） */
    uint8_t  rsv;
} GkbTok;

typedef struct {
    int16_t a, b;      /* merge 的两个 token id，数组下标即优先级 */
} GkbMerge;

typedef struct {
    int32_t     vocab;
    int32_t     n_merge;
    int32_t     bos, eos, sep;
    GkbTok     *toks;
    const uint8_t *heap;
    GkbMerge   *merges;
} GkbVocab;

/* 需要的内存字节数（保守按 vocab=4096/merges=4096 估）。 */
int  gkb_vocab_bytes(int32_t vocab, int32_t merges);

/*
 * 从 SD 卡加载（GVB1 纯文本）。返回 0 成功，负数是错误码：
 *   -1 工作区太小  -2 文件打不开  -3 格式非法
 * 词表通常已经在 .cia 里了，这条留给"用户想换一份更大的词表"的场景。
 */
int  gkb_vocab_load(const char *path, GkbVocab *dst, void *work, int work_sz);

/*
 * 从内存里的 GVB1 文本加载。guide_vocab_bin.h（构建期生成）走这条。
 * 真正的解析在这里，上面那个文件版只是薄封装 —— 走 stdio 的
 * fscanf/fgetc 逐段读，既躲不开 CRLF，又让格式错误只能靠 errno 猜。
 * 返回 0 成功，负数是错误码（同上，另有 -4 表示与预估容量不符）。
 */
int  gkb_vocab_load_mem(const char *src, int src_len,
                        GkbVocab *dst, void *work, int work_sz);

/*
 * 从内嵌词表加载（.cia 里那一份）。上线走这条。
 * 返回 0 成功，负数是错误码。
 */
int  gkb_vocab_load_embedded(GkbVocab *dst, void *work, int work_sz);

/* 特殊 token id */
int  nnet_bos_id(void);
int  nnet_eos_id(void);
int  nnet_sep_id(void);

/* 取某个 token id 的 UTF-8 文本。返回字节长度，id 越界/未加载返回 0。
 * guide_kb.c 的停用词判定依赖它：停用词必须在 **token 层面**判，
 * 因为 BPE 会把「在哪里」粘成一个独立 token，按空格切词看不到。 */
int  nnet_vocab_piece(int32_t id, const uint8_t **out);

#ifdef __cplusplus
}
#endif
#endif /* NNET_TEXT_H */