/*-
 * Copyright (c) 2025 Izumi Tsutsui.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mml_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <ctype.h>
#include <err.h>

/* バッファサイズ (サイズは適当) */
#define CH_BUF_SIZE	32768
#define LINE_BUF_SIZE	400

/* MMLコンパイル結果オブジェクトファイル構造 */
#define CH1_ADDR_OFFSET		0
#define CH2_ADDR_OFFSET		2
#define CH3_ADDR_OFFSET		4
#define CH1_START_OFFSET	8	/* オリジナルZ80版コンパイラ準拠 */

#define PSG_NCH 3

static const uint16_t ch_offset[PSG_NCH] = {
    CH1_ADDR_OFFSET, CH2_ADDR_OFFSET, CH3_ADDR_OFFSET
};

static const char *progname;

typedef struct psgch {
    MML_Compiler mmlcp;
    uint8_t *buf;
    uint16_t offset;
    char last_line[LINE_BUF_SIZE];
} psgch_t;

static void
usage(void)
{
    fprintf(stderr,
"使い方: %s [-b addr] 入力MMLファイル 出力バイナリファイル\n"
"         -b addr コンパイル後データのベースアドレス\n",
       progname);
    exit(EXIT_FAILURE);
}

/* MMLコンパイルデータの各チャンネル開始アドレス格納用 */
static void
put_word_le(uint8_t *buf, uint16_t v)
{
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)(v >> 8);
}

/* MMLコンパイルエラー表示 */
static void
print_mmlc_error(MML_Compiler *c, char *line)
{
    fprintf(stderr, "エラー: %s\n", c->error_msg);
    fprintf(stderr, "%s", line);
    fprintf(stderr, "%*s^\n", c->error_col - 1, "");
}

int
main(int argc, char *argv[])
{
    char *progpath;
    int ch;
    int baseaddr = 0x0000;
    const char *ifname, *ofname;
    FILE *ifp = NULL, *ofp = NULL;

    progpath = strdup(argv[0]);
    progname = basename(progpath);

    while ((ch = getopt(argc, argv, "b:")) != -1) {
        char *endptr;
        switch (ch) {
        case 'b':
            baseaddr = (int)strtol(optarg, &endptr, 0);
            if (*endptr != '\0' || baseaddr < 0 || baseaddr > 0xffff) {
                usage();
            }
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 2)
        usage();

    ifname = argv[0];
    ofname = argv[1];
    ifp = fopen(ifname, "r");
    if (ifp == NULL) {
        errx(EXIT_FAILURE, "入力MMLファイルを開けませんでした: %s", ifname);
    }

    psgch_t psgch[PSG_NCH];
    for (int i = 0; i < PSG_NCH; i++) {
        psgch_t *psgchp = &psgch[i];
        MML_Compiler *c = &psgchp->mmlcp;
        psgchp->buf = malloc(CH_BUF_SIZE);
        if (psgchp->buf == NULL)
            errx(EXIT_FAILURE, "コンパイル出力バッファが確保できませんでした");
        mml_channel_init(c, psgchp->buf, CH_BUF_SIZE);
        psgchp->last_line[0] = '\0';
    }

    char line[LINE_BUF_SIZE];
    int lineno = 0;
    MML_Error error;
    bool abort = false;
    bool x_disabled = false;

    /* 行単位コンパイル処理 */
    while (fgets(line, sizeof(line), ifp) != NULL) {
        lineno++;
        char *p = line;

        /* 行頭の空白とタブをスキップ */
        while (*p == ' ' || *p == '\t')
            p++;
        /* 先頭が数字なら `[行番号] "` のオリジナルコンパイラ書式も読み飛ばす */
        if (isdigit((int)(unsigned char)*p)) {
            /* 行番号相当の数字を読み飛ばす (わざわざ値はチェックしない) */
            while (isdigit((int)(unsigned char)*p))
                p++;
            /* 行番号後に空白をスキップ */
            while (*p == ' ')
                p++;
            /* 二重引用符 `"` をスキップ */
            if (*p == '"')
                p++;
        }
        ch = toupper((int)(unsigned char)*p);
        for (int i = 0; i < PSG_NCH; i++) {
            if (!x_disabled && ch == 'D' + i) {
                psgch_t *psgchp = &psgch[i];
                MML_Compiler *c = &psgchp->mmlcp;
                error = mml_compile_line(c, p + 1, lineno);
                if (error != MML_OK) {
                    print_mmlc_error(c, line);
                    abort = true;
                }
                /* クローズ後のエラーメッセージ用に最終行を保存 */
                strncpy(psgchp->last_line, line, sizeof(psgchp->last_line));
                psgchp->last_line[sizeof(psgchp->last_line) - 1] = '\0';
            }
        }
        if (ch == 'X') {
            /* 行頭の'X'チャンネル指定はコンパイル停止/再開 */
            x_disabled = !x_disabled;
        } else {
            DPRINTF("ignored line %d\n", lineno);
        }
    }
    fclose(ifp);

    /* 全行コンパイル後にチャンネルクローズしてエラーチェック */
    for (int i = 0; i < PSG_NCH; i++) {
        psgch_t *psgchp = &psgch[i];
        MML_Compiler *c = &psgchp->mmlcp;
        error = mml_finish_channel(c);
        if (error != MML_OK) {
            print_mmlc_error(c, psgchp->last_line);
            abort = true;
        }
        DPRINTF("psgch[%d].out_len = %d\n", i, c->out_len);
    }

    if (abort) {
        errx(EXIT_FAILURE, "コンパイルエラーのため出力せず終了します");
    }

    /* チャンネルデータ出力 */
    ofp = fopen(ofname, "wb");
    if (ofp == NULL) {
        errx(EXIT_FAILURE,
          "出力コンパイルバイナリファイルを開けませんでした: %s", ofname);
    }

    psgch[0].offset = CH1_START_OFFSET;
    psgch[1].offset = psgch[0].offset + psgch[0].mmlcp.out_len;
    psgch[2].offset = psgch[1].offset + psgch[1].mmlcp.out_len;
    int totallen    = psgch[2].offset + psgch[2].mmlcp.out_len;

    DPRINTF("ch1 offset = %d\n", psgch[0].offset);
    DPRINTF("ch2 offset = %d\n", psgch[1].offset);
    DPRINTF("ch3 offset = %d\n", psgch[2].offset);
    DPRINTF("totallen   = %d\n", totallen);

    uint8_t *outbuf = malloc(totallen);
    if (outbuf == NULL) {
        errx(EXIT_FAILURE, "出力バッファを確保できませんでした");
    }

    /* 出力バッファにコンパイル結果をセット */
    for (int i = 0; i < PSG_NCH; i++) {
        psgch_t *psgchp = &psgch[i];
        MML_Compiler *c = &psgchp->mmlcp;
        put_word_le(outbuf + ch_offset[i], baseaddr + psgchp->offset);
        memcpy(outbuf + psgchp->offset, c->out, c->out_len);
        free(psgchp->buf);
    }

    /* 出力バッファ書き込み */
    if (fwrite(outbuf, 1, totallen, ofp) != totallen) {
        errx(EXIT_FAILURE, "出力ファイルの書き込みに失敗しました");
    }

    free(outbuf);
    fclose(ofp);
    free(progpath);

    exit(EXIT_SUCCESS);
}
