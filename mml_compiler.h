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

#ifndef MML_COMPILER_H
#define MML_COMPILER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* エラーメッセージ種別 (結局使ってない) */
typedef enum {
    MML_OK = 0,
    MML_ERR_SYNTAX,
    MML_ERR_FUNC_RANGE,
    MML_ERR_OCTAVE,
    MML_ERR_OUT_OF_NEST,
    MML_ERR_CLOSE_NEST,
    MML_ERR_DUP_EXIT,
    MML_ERR_RETURN_IN_NEST,
    MML_ERR_NOTE_OVERFLOW,
    MML_ERR_INTERNAL
} MML_Error;

/* '[', ':', ']' の各コマンドのループ状態管理用 */
typedef struct {
    size_t loop_start;     /* ネスト '['コマンド位置 */
#define LOOP_NOEXIT 0x0000
    size_t exit_mark;      /* 対応する ':'コマンド位置 (無ければ LOOP_NOEXIT) */

    /* オリジナルコンパイラは以下が1組だけだったので多重ネストで壊れていた? */
    int saved_l_len96;     /* L音長 退避用 */
    int saved_lp_len96;    /* L+音長 退避用 */
    int saved_octave;      /* オブジェクト上のオクターブ 退避用 */
    int saved_octave_last; /* 1つ前の音符のオクターブ 退避用 */
} MML_LoopState;

#define MML_MAX_NEST 4

typedef struct {
    /* --- 入力行情報 (各行コンパイル時に初期化) --- */
    const char *src;
    size_t      pos;
    size_t      len;
    int         line;
    int         col;

    /* --- 出力オブジェクトバッファ (全行共通の追記バッファ) --- */
    uint8_t *out;
    size_t   out_len;
    size_t   out_cap;

    /* --- チャンネル状態 (コンパイル全体で継続して保持) --- */
    int nest_depth;
    int l_len96;          /* L で指定された音長 (96分音符単位) */
    int lp_len96;         /* L+ で指定された音長 (96分音符単位) */
    int octave;           /* オブジェクト上のオクターブ */
    int octave_last;      /* １つ前の音符のオクターブ (転調分含む) */
    int key_shift;        /* 転調指定値 */

    /* --- ループ状態管理 --- */
    MML_LoopState loops[MML_MAX_NEST];

    /* --- コンパイルエラー情報 --- */
    MML_Error error;
#define NOERROR (-1)
    int       error_col;
    char      error_msg[128];
} MML_Compiler;

/* --- 公開API ------------------------------------------------------------- */
/* チャンネル別データ初期化 */
void mml_channel_init(MML_Compiler *c, uint8_t *out_buf, size_t out_size);

/* 行単位チャンネル別コンパイル */
MML_Error mml_compile_line(MML_Compiler *c, const char *src, int line_no);

/* チャンネル終了処理 */
MML_Error mml_finish_channel(MML_Compiler *c);

/* デバッグ用定義 */
#ifdef DEBUG
#define DPRINTF(...)	(void)fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...)	((void)0)
#endif

#endif /* MML_COMPILER_H */
