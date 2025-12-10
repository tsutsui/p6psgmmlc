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

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>

static int  peek(MML_Compiler *c);
static int  get(MML_Compiler *c);
static void skip_space(MML_Compiler *c);
static int  parse_unsigned(MML_Compiler *c, int *out);
static int  parse_signed(MML_Compiler *c, int *out);
static int  sign_byte(int v);
static void set_error(MML_Compiler *c, MML_Error e, const char *msg);
static int  ensure_space(MML_Compiler *c, size_t need);
static void emit_byte(MML_Compiler *c, uint8_t v);
static void emit_word_le(MML_Compiler *c, uint16_t v);

static int  notename_to_tonenum(char name);
static void parse_para(MML_Compiler *c, uint8_t *flagp, uint16_t *valuep);
static int  parse_length_96(MML_Compiler *c, int *len96, uint8_t *flagp);
static int  apply_dots(MML_Compiler *c, int base_len96, int dots, int *out_len96);
static uint8_t make_note_header(MML_Compiler *c, int tone, int len96, int tie);

static void set_octave(MML_Compiler *c, int n);
static void emit_octave(MML_Compiler *c, int n);

static void compile_statement(MML_Compiler *c);
static void compile_note(MML_Compiler *c, int note);
static void compile_command(MML_Compiler *c, int command);

/* --- 公開API ------------------------------------------------------------- */

/*
 * チャンネル別データ初期化
 *  MMLCompiler *c: 入力バッファ情報他
 *  out_buf: コンパイル出力結果
 *  out_size: out_bufサイズ
 */
void
mml_channel_init(MML_Compiler *c, uint8_t *out_buf, size_t out_size)
{
    memset(c, 0, sizeof(*c));

    c->out     = out_buf;
    c->out_len = 0;
    c->out_cap = out_size;

    /* チャンネル状態の初期値 (ドライバ仕様に合わせる) */
    c->l_len96     = 24;        /* L音長  4分音符 相当 */
    c->lp_len96    = 192;       /* L+音長 全音符×2 相当 */
    c->octave      = 4;         /* ドライバ側初期値を仮定 */
    c->octave_last = c->octave; 
    c->key_shift   = 0;

    c->nest_depth = 0;
    for (int i = 0; i < MML_MAX_NEST; i++) {
        c->loops[i].loop_start        = 0;
        c->loops[i].exit_mark         = LOOP_NOEXIT;
        c->loops[i].saved_l_len96     = 0;
        c->loops[i].saved_lp_len96    = 0;
        c->loops[i].saved_octave      = 0;
        c->loops[i].saved_octave_last = 0;
    }

    c->error = MML_OK;
    c->error_msg[0] = '\0';

    /* 行入力用フィールドはまだ設定しない */
    c->src  = NULL;
    c->pos  = 0;
    c->len  = 0;
    c->line = 0;
    c->col  = 0;
}

/*
 * 行単位チャンネル別コンパイル
 *  1行分のMMLをコンパイルして既存の出力バッファに追加する
 *  MMLCompiler *c: 入力バッファ情報 他
 *  const char *src: コンパイル対象チャンネル別入力行
 *  line_no: コンパイル対象の行番号
 */
MML_Error
mml_compile_line(MML_Compiler *c, const char *src, int line_no)
{
    c->src  = src;
    c->len = strlen(src);
    c->pos  = 0;
    c->line = line_no;
    c->col  = 1;

    c->error = MML_OK;
    c->error_col = NOERROR;
    c->error_msg[0] = '\0';

    while (c->pos < c->len && c->error == MML_OK) {
        compile_statement(c);
    }

    return c->error;
}

/*
 * チャンネル終了処理
 *  全行読み終わったあとに呼び出してネストチェック後に終端マークを付与する
 */
MML_Error
mml_finish_channel(MML_Compiler *c)
{
    /* ネストが閉じているか最終チェック */
    if (c->nest_depth != 0) {
        set_error(c, MML_ERR_CLOSE_NEST,
          "ネストを閉じないままチャンネルが終了しました");
        return c->error;
    }

    /* 出力末尾にエンドマーク 0xFF を付加 */
    emit_byte(c, 0xFF);
    c->error = MML_OK;
    return MML_OK;
}

/* --- バッファ処理ヘルパ関数 ---------------------------------------------- */

/* 入力 1文字チェック */
static int
peek(MML_Compiler *c)
{
    if (c->pos >= c->len)
        return -1;
    int ch = (unsigned char)c->src[c->pos];
    return ch;
}

/* 入力 1文字読み出し */
static int
get(MML_Compiler *c)
{
    if (c->pos >= c->len)
        return -1;
    int ch = (unsigned char)c->src[c->pos++];
    if (ch == '\n') {
        /* 改行チェックは別で実施される前提でそのまま返す */
        return ch;
    } else {
        c->col++;
    }
    return ch;
}

/* 入力のスペースやタブを読み捨て */
static void
skip_space(MML_Compiler *c)
{
    for (;;) {
        int ch = peek(c);
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            (void)get(c);
            continue;
        }
        break;
    }
}

/* 入力から符号なし数字列から数値を取り出し */
static int
parse_unsigned(MML_Compiler *c, int *out)
{
    skip_space(c);
    int ch = peek(c);
    if (!isdigit(ch))
        return 0;
    long v = 0;
    while (isdigit((ch = peek(c)))) {
        (void)get(c);
        v = v * 10 + (ch - '0');
        if (v > INT_MAX) v = INT_MAX;
    }
    *out = (int)v;
    return 1;
}

/* 入力から符号あり数字列から数値を取り出し */
static int
parse_signed(MML_Compiler *c, int *out)
{
    skip_space(c);
    int ch = peek(c);
    int sign = 1;
    if (ch == '+' || ch == '-') {
        sign = (ch == '-') ? -1 : 1;
        (void)get(c);
    }
    int v;
    if (!parse_unsigned(c, &v))
        return 0;
    *out = sign * v;
    return 1;
}

/* 符号付き1バイトをbit7を符号、bit6-0 を絶対値でエンコード */
static int
sign_byte(int v)
{
    return (v >= 0) ? v : (0x80 | -v);
}

/* エラー文字列と発生箇所を共通構造体にセット */
static void
set_error(MML_Compiler *c, MML_Error e, const char *msg)
{
    if (c->error == MML_OK) {
        c->error = e;
        if (msg) {
            if (c->error_col == NOERROR)
                c->error_col = c->col;
            snprintf(c->error_msg, sizeof(c->error_msg),
              "%s (%d 行目, %d 桁目)",
              msg, c->line, c->error_col);
        }
    }
}

/* 出力バッファサイズチェック */
static int
ensure_space(MML_Compiler *c, size_t need)
{
    if (c->out_len + need > c->out_cap) {
        c->error_col = c->col;
        set_error(c, MML_ERR_INTERNAL,
          "コンパイル結果出力サイズがバッファサイズを超えました");
        return 0;
    }
    return 1;
}

/* 出力バッファ 1バイト出力 */
static void
emit_byte(MML_Compiler *c, uint8_t v)
{
    if (!ensure_space(c, 1))
        return;
    c->out[c->out_len++] = v;
}

/* 出力バッファ 2バイト出力 (LSB First) */
static void
emit_word_le(MML_Compiler *c, uint16_t v)
{
    if (!ensure_space(c, 2))
        return;
    c->out[c->out_len++] = (uint8_t)(v & 0xFF);
    c->out[c->out_len++] = (uint8_t)(v >> 8);
}

/* --- 音符・休符・Lコマンド音長用ヘルパ関数 ------------------------------- */

/* ノート文字列からノート番号へ変換 */
static int
notename_to_tonenum(char name)
{
    /* C=1, C#=2, D=3, ... B=12 */
    switch (toupper((unsigned char)name)) {
    case 'C':
        return 1;
    case 'D':
        return 3;
    case 'E':
        return 5;
    case 'F':
        return 6;
    case 'G':
        return 8;
    case 'A':
        return 10;
    case 'B':
        return 12;
    default:
        break;
    }
    return -1;
}

/*
 * PARA 相当: プレフィクス ('%', '+', '-') と数字列を読む共通ルーチン
 *  ここはオリジナルZ80版の解析も複雑なので結果を細かく記載しておく
 *
 * 入力:
 *   - c->line, c->pos: 解析開始位置。
 *       line[pos] は、Z80版でいうところの
 *       「INC HL した後に A = (HL) した文字」と同じイメージ。
 *       つまり、コマンド文字 'L', 'R', 'C' などの「次の文字」から渡す。
 *
 * 出力（成功時）:
 *   - *valuep : 読み取った数値 (0〜65535, オーバーフロー未定義)
 *   - *flagp :
 *       bit7: '-' フラグ
 *       bit6: '+' フラグ
 *       bit5: '%' フラグ
 *       bit4: タイフラグ (PARA 内では変更しない; C版では未使用)
 *       bit0: 0 （少なくとも1桁数字を読んだ）
 *   - c->pos : 数字列の直後の文字位置まで進められる
 *
 * 出力（失敗時 = 数字が1桁も無い）:
 *   - *flags : bit0 が 1 にセットされる
 *              （他の bit5/6/7 は、見つかった物に応じてセットされ得る）
 *   - c->pos : 「数字でなかった文字」の位置に進められる
 *              （呼び出し側でその文字を再解釈可能）
 *
 * その他:
 *   - 範囲チェック（例: L の許容値、Q=0〜255 など）は呼び出し側で行う。
 *   - 「関数コールエラー / Function Call Error」かどうかの判断も呼び出し側。
 */

#define PARA_F_MINUS	0x80
#define PARA_F_PLUS	0x40
#define PARA_F_PERCENT	0x20
#define PARA_F_TIE	0x10
#define PARA_F_NOVALUE	0x01

static void
parse_para(MML_Compiler *c, uint8_t *flagp, uint16_t *valuep)
{
    uint8_t flag = 0;
    uint16_t value = 0;
    int ch;

    skip_space(c);
    ch = peek(c);
    /* まず '%' 有無をチェック */
    if (ch == '%') {
        flag |= PARA_F_PERCENT;
        (void)get(c);
        skip_space(c);
    }

    /* 次に '+' or '-' 有無をチェック */
    ch = peek(c);
    if (ch == '-') {
        flag |= PARA_F_MINUS;
        (void)get(c);
        skip_space(c);
    } else if (ch == '+') {
        flag |= PARA_F_PLUS;
        (void)get(c);
        skip_space(c);
    }

    /* 少なくとも1桁数字があるのをチェック */
    ch = peek(c);
    if (!isdigit(ch)) {
        flag |= PARA_F_NOVALUE;
        goto out;
    }
    for (;;) {
        ch = peek(c);
        if (!isdigit(ch)) {
            /* 数字以外が来たところで終了 */
            break;
        }
        uint32_t digit = (uint32_t)ch - '0';
        value = value * 10U + digit;
        if (value >= UINT16_MAX)
            value = UINT16_MAX;
        (void)get(c);
    }

 out:
    if (valuep != NULL)
        *valuep = value;
    if (flagp != NULL)
        *flagp = flag;
}

/* 長さ n もしくは %n ('.' 付点と '^' 連結含む) を96分音符単位音長に変換 */
static int
parse_length_96(MML_Compiler *c, int *len96, uint8_t *flagp)
{
    uint8_t flag;
    uint16_t value;
    int base_len = 0;
    int ch;

    parse_para(c, &flag, &value);
    c->error_col = c->col;
    /* PARA_F_PLUS と PARA_F_MINUS は呼び出し側でチェック */
    if ((flag & PARA_F_PERCENT) != 0) {
        /* %n 音長直接指定 */
        if ((flag & PARA_F_NOVALUE) != 0) {
            /* これ、デフォルト L 音長にすべき? */
            set_error(c, MML_ERR_FUNC_RANGE,
                "音長の'%'に数値指定がありません");
            return 0;
        }
        if (value < 1 || value > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "音長の'%'の値が不正です (1〜255)");
            return 0;
        }
        base_len = value;
    } else {
        if ((flag & PARA_F_NOVALUE) != 0) {
            /* 音長省略: L のデフォルトを使用 */
            base_len = c->l_len96;
        } else {
            /* n分音符について、最小単位が96分音符の約数でないとダメ */
            switch (value) {
                case 1:
                case 2:
                case 3:
                case 4:
                case 6:
                case 8:
                case 12:
                case 16:
                case 24:
                case 32:
                case 48:
                case 96:
                    base_len = 96 / value;
                    break;
                default:
                    set_error(c, MML_ERR_FUNC_RANGE,
                      "音長の値が不正です (1,2,3,4,6,8,12,16,24,32,48,96)");
                    return 0;
            }
        }
    }

    /* ドット処理: '.' が続く数を数える */
    int dots = 0;
    c->error_col = c->col;
    for (;;) {
        skip_space(c);
        ch = peek(c);
        if (ch == '.') {
            (void)get(c);
            dots++;
        } else {
            break;
        }
    }
    if (dots > 0) {
        c->error_col++;;
        if (!apply_dots(c, base_len, dots, &base_len)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "演奏できない音長になる付点が指定されています");
            return 0;
        }
    }
    c->error_col = NOERROR;

    /* '^' による合算 */
    for (;;) {
        skip_space(c);
        ch = peek(c);
        if (ch != '^')
            break;
        (void)get(c);
        int add_len = 0;
        /* えいやで再帰 */
        if (!parse_length_96(c, &add_len, NULL)) {
            return 0;
        }
        base_len += add_len;
    }

    *len96 = base_len;
    if (flagp != NULL)
        *flagp = flag;
    return 1;
}

/* ベース音長に対して dots個のドット分の 1/2, 1/4, 1/8... を加算 */
static int
apply_dots(MML_Compiler *c, int base_len96, int dots, int *out_len96)
{
    int len96 = base_len96;
    int half = base_len96;
    for (int i = 0; i < dots; i++) {
        /* すでに奇数なら . 付与不可 */
        if ((half % 2) != 0)
            return 0;
        half /= 2;
        len96 += half;
        c->error_col++;
    }

    if (len96 <= 0 || len96 > 32767)
        return 0;
    *out_len96 = len96;
    return 1;
}

/* ノートと音長と L音長/L+音長から bit5-4 を決めて音符コマンドヘッダを作る */
static uint8_t
make_note_header(MML_Compiler *c, int tone, int len96, int tie)
{
    uint8_t header = 0;

    /* bit7 = 0 (音符・休符) */
    /* bit6 = tie */
    header |= (tie ? 1 : 0) << 6;

    /* bit5-4: L / L+ / 1byte / 2byte */
    if (len96 == c->l_len96) {
        /* bit5-4 == 0b00: 音長なし音符 (Lコマンド音長) */
        header |= 0x0 << 4;
    } else if (len96 == c->lp_len96) {
        /* bit5-4 == 0b01: 音長なし音符 (L+コマンド音長) */
        header |= 0x1 << 4;
    } else if (len96 <= 255) {
        /* bit5-4 == 0b10: 音長あり音符 (音長1バイト) */
        header |= 0x2 << 4;
    } else {
        /* bit5-4 == 0b11: 音長あり音符 (音長2バイト) */
        header |= 0x3 << 4;
    }

    /* 下位4bit: 音種別 (0=休符, 1=C, 2=D, .., 12=B) */
    header |= (uint8_t)tone & 0x0F;

    return header;
}

/* --- コマンド出力ヘルパ関数 ---------------------------------------------- */

/* オクターブ出力; 範囲チェックを集約 */
static void
set_octave(MML_Compiler *c, int n)
{
    if (n < 1 || n > 8) {
        set_error(c, MML_ERR_OCTAVE,
          "オクターブの設定値が範囲外です (1〜8)");
        return;
    }
    c->octave = n;
}

static void
emit_octave(MML_Compiler *c, int n)
{
    if (n < 1 || n > 8) {
        set_error(c, MML_ERR_OCTAVE,
          "オクターブの出力値が範囲外です (1〜8)");
        return;
    }
    emit_byte(c, 0x80 + (uint8_t)n);
    c->octave_last = n;
}

/* --- メイン行単位パーサー ------------------------------------------------ */

/* 1 文（コマンド or 音符 or コメント）をパースしてそれぞれ処理 */
static void
compile_statement(MML_Compiler *c)
{
    skip_space(c);
    int ch = peek(c);
    if (ch < 0)
        return;

    if (ch == ';') {
        /* コメント: 行末まで読み飛ばし */
        while ((ch = get(c)) >= 0 && ch != '\n')
            /* nothing */;
        return;
    }

    if (ch == '\n') {
        (void)get(c);
        return;
    }

    ch = get(c);
    int up = toupper(ch);

    DPRINTF("ch = '%c' (up = '%c')\n", ch, up);

    if (strchr("ABCDEFGR", up)) {
        /* 音符・休符処理 */
        compile_note(c, up);
    } else {
        /* コマンド処理 */
        compile_command(c, up);
    }
}

/* 音符・休符処理 */
static void
compile_note(MML_Compiler *c, int note)
{
    /* 呼び出し側で大文字にしているが念の為 */
    int ch = toupper(note);
    int octave = c->octave;
    int tone;
    c->error_col = c->col;
    if (ch == 'R') {
        tone = 0; /* rest */
    } else {
        tone = notename_to_tonenum((char)ch);
        if (tone < 0) {
            /* 事前にA〜Gをチェックしてるのでここには来ない */
            set_error(c, MML_ERR_SYNTAX,
              "不正な音符データです (A〜B)");
            return;
        }
        /* #,+,- 処理 */
        skip_space(c);
        ch = peek(c);
        if (ch == '#' || ch == '+') {
            tone++;;
            (void)get(c);
        } else if (ch == '-') {
            tone--;
            (void)get(c);
        }
        if (tone > 12)
            tone = 12; /* b+ は b にクリップ */
        if (tone < 1)
            tone = 1;  /* c- は c にクリップ */

        /* 転調分の調整 */
         if (c->key_shift != 0) {
            tone += c->key_shift;
            if (tone > 12) {
                octave++;
                tone -= 12;
            } else if (tone < 1) {
                octave--;
                tone += 12;
            }
            if (octave < 1 || octave > 8) {
                set_error(c, MML_ERR_NOTE_OVERFLOW,
                  "転調後の音符が範囲外です");
                return;
            }
        }
    }
    c->error_col = NOERROR;

    /* 音長チェック */
    int len96;
    uint8_t flag;
    if (!parse_length_96(c, &len96, &flag))
        return;
    if ((flag & PARA_F_PLUS) != 0) {
        set_error(c, MML_ERR_FUNC_RANGE,
          "音長に'+'は指定できません");
        return;
    }
    if ((flag & PARA_F_MINUS) != 0) {
        set_error(c, MML_ERR_FUNC_RANGE,
          "音長に'-'は指定できません");
        return;
    }

    /* オリジナルコンパイラはタイをPARAで見ているがここでは別で見る */
    int tie = 0;
    skip_space(c);
    if (peek(c) == '&') {
        (void)get(c);
        tie = 1;
    }

    if (c->octave_last != octave) {
        emit_octave(c, octave);
    }
    uint8_t onpu = make_note_header(c, tone, len96, tie);
    emit_byte(c, onpu);

    /* L / L+ と一致しないときだけ長さバイトを出す */
    if (len96 != c->l_len96 && len96 != c->lp_len96) {
        if (len96 <= 255) {
            /* 音長1バイト */
            emit_byte(c, (uint8_t)len96);
        } else {
            /* 音長2バイト */
            emit_word_le(c, (uint16_t)len96);
        }
    }
}

/* コマンド処理 */
static void
compile_command(MML_Compiler *c, int command)
{
    /* 呼び出し側で大文字にしているが念の為 */
    int ch = toupper(command);
    /* コマンド: 1 文字で dispatch */
    switch (ch) {
    case 'O': { /* オクターブ (1〜8) */
        int v;
        if (!parse_unsigned(c, &v)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'O'コマンドに数値指定がありません");
            return;
        }
        /* ここでは現在のオクターブを更新するだけ */
        /* 音符出力時にオクターブ変化していた時にオクターブコマンドを出力 */
        set_octave(c, v);
        break;
    }
    case '>': { /* オクターブをn上げる。n省略で1つ上げる (1〜8) */
        int v;
        if (!parse_unsigned(c, &v)) {
            c->error_col = c->col - 1;
            v = 1;
        }
        /* ここでは現在のオクターブを更新するだけ */
        set_octave(c, c->octave + v);
        break;
    }
    case '<': { /* オクターブをn下げる。n省略で1つ下げる (1〜8) */
        int v;
        if (!parse_unsigned(c, &v)) {
            c->error_col = c->col - 1;
            v = 1;
        }
        /* ここでは現在のオクターブを更新するだけ */
        set_octave(c, c->octave - v);
        break;
    }
    case 'V': { /* ボリューム (0〜15) */
        int v;
        if (!parse_unsigned(c, &v)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'V'コマンドに数値指定がありません");
            return;
        }
        if (v < 0 || v > 15) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'V'コマンドの値が範囲外です (0〜15)");
            return;
        }
        emit_byte(c, 0x90 + (uint8_t)v);
        break;
    }
    case '(' : { /* ボリュームをn上げる。n省略で１つ上げる。 (1〜15) */
        int v;
        if (!parse_unsigned(c, &v))
            v = 1;
        if (v < 1 || v > 15) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'('コマンドの値が範囲外です (1〜15)");
            return;
        }
        emit_byte(c, 0xB0 + (uint8_t)v);
        break;
    }
    case ')' : { /* ボリュームをn下げる。n省略で１つ下げる。 (1〜15) */
        int v;
        if (!parse_unsigned(c, &v)) v = 1;
        if (v < 1 || v > 15) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "')'コマンドの値が範囲外です (1〜15)");
            return;
        }
        emit_byte(c, 0xA0 + (uint8_t)v);
        break;
    }
    case 'I': { /* 変数nをワークエリアに書き込む (0〜255) */
        int v;
        if (!parse_unsigned(c, &v)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'I'コマンドの数値指定がありません");
            return;
        }
        if (v < 0 || v > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'I'コマンドの値が範囲外です (0〜255)");
            return;
        }
        emit_byte(c, 0xF4);
        emit_byte(c, (uint8_t)v);
        break;
    }
    case 'J': { /* 演奏データが最終まできたらこの地点まで戻る */
        if (c->nest_depth > 0) {
            set_error(c, MML_ERR_RETURN_IN_NEST,
              "'J'コマンドはネスト中に指定できません");
             /* 解析上はこのあとのネスト終了をパースできないので一旦リセット */
            c->nest_depth = 0;
           return;
        }
        emit_byte(c, 0xFE);
        break;
    }
    case 'L': { /* 音長設定。nは音長に準ずる (L+n の場合は L+音長設定) */
        int len96;
        uint8_t flag;   /* L+ かどうかの判定用 */
        if (!parse_length_96(c, &len96, &flag)) {
            return;
        }

        if ((flag & PARA_F_NOVALUE) != 0) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'L'コマンドに数値指定がありません");
            return;
        }
        if ((flag & PARA_F_MINUS) != 0) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'L'コマンドに'-'は使用できません");
            return;
        }
        if (len96 < 0 || len96 > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'L'コマンドの値が範囲外です (1〜255)");
            return;
        }
        if ((flag & PARA_F_PLUS) == 0) {
            /* L音長 */
            c->l_len96 = len96;
            emit_byte(c, 0xF9);           /* L コマンド */
        } else {
            /* L+音長 */
            c->lp_len96 = len96;
            emit_byte(c, 0xF7);           /* L+ コマンド */
        }
        emit_byte(c, (uint8_t)len96); /* パラメータは L / L+ 共通で音長 */
        break;
    }

    case 'M': { /* ビブラート (M%n の場合は第4パラメータのみセット) */
        skip_space(c);
        int nxt = peek(c);
        if (nxt == '%') {
            (void)get(c);
            int v;
            if (!parse_signed(c, &v)) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'M%%'コマンドの数値指定がありません");
                return;
            }
            if (v < -127 || v > 127) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'M%%'コマンドの値が範囲外です (-127〜127)");
                return;
            }
            v = sign_byte(v);
            emit_byte(c, 0xFD);
            emit_byte(c, (uint8_t)v);
        } else {
            int n1, n2, n3, n4;
            if (!parse_unsigned(c, &n1))
                goto func_err;
            skip_space(c);
            if (peek(c) != ',')
                goto func_err;
            (void)get(c);
            if (!parse_unsigned(c, &n2))
                goto func_err;
            skip_space(c);
            if (peek(c) != ',')
                goto func_err;
            (void)get(c);
            if (!parse_unsigned(c, &n3))
                goto func_err;
            skip_space(c);
            if (peek(c) != ',')
                goto func_err;
            (void)get(c);
            if (!parse_signed(c, &n4))
                goto func_err;
            n4 = sign_byte(n4);
            /* 範囲チェックはざっくり */
            emit_byte(c, 0xF5);
            emit_byte(c, (uint8_t)n1);
            emit_byte(c, (uint8_t)n2);
            emit_byte(c, (uint8_t)n3);
            emit_byte(c, (uint8_t)n4);
        }
        break;
    func_err:
        set_error(c, MML_ERR_FUNC_RANGE,
          "'M'コマンドのパラメータが不正です");
        return;
    }
    case 'N': { /* ビブラート効果の有効／無効スイッチ */
        emit_byte(c, 0xF6);
        break;
    }
    case 'P': { /* ノイズモード設定 (1〜3) */
        int v;
        if (!parse_unsigned(c, &v)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'P'コマンドの数値指定がありません");
            return;
        }
        if (v == 1)
            emit_byte(c, 0xED);
        else if (v == 2)
            emit_byte(c, 0xEE);
        else if (v == 3)
            emit_byte(c, 0xEF);
        else {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'P'コマンドの値が範囲外です (1,2,3)");
            return;
        }
        break;
    }
    case 'Q': { /* ゲートタイム (0〜255) */
        int v;
        if (!parse_unsigned(c, &v)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'Q'コマンドの数値指定がありません");
            return;
        }
        if (v < 0 || v > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'Q'コマンドの値が範囲外です (0〜255)");
            return;
        }
        emit_byte(c, 0xFA);
        emit_byte(c, (uint8_t)v);
        break;
    }
    case 'S': { /* ソフトウェアエンベロープ */
        int n1, n2, n3, n4, n5;
        if (!parse_signed(c, &n1))
            goto s_err;
        skip_space(c);
        if (peek(c) != ',')
            goto s_err;
        (void)get(c);
        if (!parse_unsigned(c, &n2))
            goto s_err;
        skip_space(c);
        if (peek(c) != ',')
            goto s_err;
        (void)get(c);
        if (!parse_signed(c, &n3))
            goto s_err;
        skip_space(c);
        if (peek(c) != ',')
            goto s_err;
        (void)get(c);
        if (!parse_signed(c, &n4))
            goto s_err;
        skip_space(c);
        if (peek(c) != ',')
            goto s_err;
        (void)get(c);
        if (!parse_signed(c, &n5))
            goto s_err;
        n5 = sign_byte(n5);

        emit_byte(c, 0xEA);
        emit_byte(c, (uint8_t)n1);
        /* 第1パラメータが0、つまりエンベロープOFFのときは残りは書き込まない */
        if (n1 != 0) {
            emit_byte(c, (uint8_t)n2);
            emit_byte(c, (uint8_t)n3);
            emit_byte(c, (uint8_t)n4);
            emit_byte(c, (uint8_t)n5);
        }
        break;
    s_err:
        set_error(c, MML_ERR_FUNC_RANGE,
          "'S'コマンドのパラメータが不正です");
        return;
    }
    case 'T': { /* テンポ (n1, n2 とも 1〜255) */
        int n1, n2;
        if (!parse_unsigned(c, &n1))
            goto t_err;
        if (n1 < 1 || n1 > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'T'コマンドのn1の値が範囲外です (1〜255)");
            return;
        }
        skip_space(c);
        if (peek(c) != ',')
            goto t_err;
        (void)get(c);
        if (!parse_unsigned(c, &n2))
            goto t_err;
       if (n2 < 0 || n2 > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'T'コマンドのn2の値が範囲外です (0〜255)");
            return;
        }

        emit_byte(c, 0xF8);
        emit_byte(c, (uint8_t)n1);
        emit_byte(c, (uint8_t)n2);
        break;
    t_err:
        set_error(c, MML_ERR_FUNC_RANGE,
          "'T'コマンドのパラメータが不正です");
        return;
    }
    case 'U': { /* U%n, U+n, U-n: デチューン (-127〜127) */
        skip_space(c);
        int nxt = peek(c);
        if (nxt == '%') {
            (void)get(c);
            int v;
            if (!parse_signed(c, &v)) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'U%%'コマンドの数値指定がありません");
                return;
            }
            if (v < -127 || v > 127) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'U%%'コマンドの値が範囲外です (-127〜127)");
                return;
            }
            v = sign_byte(v);
            emit_byte(c, 0xFB);
            emit_byte(c, (uint8_t)v);
        } else if (nxt == '+' || nxt == '-') {
            int v;
            if (!parse_signed(c, &v)) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'U+/-'コマンドの数値指定がありません");
                return;
            }
            if (v < -127 || v > 127) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "U'+/-'コマンドの値が範囲外です (-127〜+127)");
                return;
            }
            emit_byte(c, 0xFC);
            emit_byte(c, (uint8_t)v);
        } else {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'U'コマンドの書式が不正です");
            return;
        }
        break;
    }
    case 'W': { /* ノイズ周波数 (0〜31) */
        skip_space(c);
        int nxt = peek(c);
        if (nxt == '+' || nxt == '-') {
            int v;
            if (!parse_signed(c, &v)) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'W+/-'コマンドの数値指定がありません");
                return;
            }
            if (v < -31 || v > 31) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'W+/-'コマンドの値が範囲外です(-31〜+31)");
                return;
            }
            emit_byte(c, 0xEC);
            emit_byte(c, (uint8_t)v);
        } else {
            int v;
            if (!parse_unsigned(c, &v)) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'W'コマンドの数値指定がありません");
                return;
            }
            if (v < 0 || v > 31) {
                set_error(c, MML_ERR_FUNC_RANGE,
                  "'W'コマンドの値が範囲外です (0〜31)");
                return;
            }
            emit_byte(c, 0xEB);
            emit_byte(c, (uint8_t)v);
        }
        break;
    }
    case 'X': { /* コンパイル停止 */
        /* ネストチェック */
        if (c->nest_depth > 0) {
            set_error(c, MML_ERR_RETURN_IN_NEST,
              "'X'コマンドはネスト中に指定できません");
            /* 解析上はこのあとのネスト終了をパースできないので一旦リセット */
            c->nest_depth = 0;
            return;
        }
        emit_byte(c, 0xE9);
        /* 残り行データをすべて読み捨てて return */
        while ((ch = peek(c)) >= 0 && ch != '\n')
            (void)get(c);
        /* XXX: 当該チャンネルのコンパイル終了を呼び出し側に通知するI/Fが未 */
        break;
    }
    case '_': { /* 転調 (-12〜12) */
        int v;
        if (!parse_signed(c, &v)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'_'コマンドの数値指定がありません");
            return;
        }
        if (v < -12 || v > 12) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "'_'コマンドの値が範囲外です (-12〜12");
            return;
        }
        c->key_shift = v;
        break;
    }
    case '[': { /* ネスト開始 */
        if (c->nest_depth >= MML_MAX_NEST) {
            set_error(c, MML_ERR_FUNC_RANGE,
            "'['コマンドのネストが深すぎます (4段まで)");
            /* 解析上はこのあとのネスト終了をパースできないので一旦リセット */
            c->nest_depth = 0;
            return;
        }
        emit_byte(c, 0xF0);
        emit_byte(c, 0x00); /* ループ回数; 後で ] 側で埋められる */
        MML_LoopState *ls = &c->loops[c->nest_depth++];
        /* ループ最後から戻る位置は [ の次のノート */
        ls->loop_start = c->out_len;
        /* 以下は : での脱出があるときに埋められる */
        ls->exit_mark  = LOOP_NOEXIT;
        ls->saved_l_len96 = 0;
        ls->saved_lp_len96 = 0;
        ls->saved_octave = 0;
        ls->saved_octave_last = 0;
        break;
    }
    case ']': { /* ネスト終了 */
        if (c->nest_depth <= 0) {
            set_error(c, MML_ERR_OUT_OF_NEST,
              "']'コマンドに対応するネスト開始'['がありません");
            return;
        }
        int count;
        if (!parse_unsigned(c, &count)) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "']'コマンドの数値指定がありません");
            return;
        }
        if (count < 2 || count > 255) {
            set_error(c, MML_ERR_FUNC_RANGE,
              "']'コマンドの値が範囲外です (2〜255)");
            return;
        }
        MML_LoopState *ls = &c->loops[c->nest_depth - 1];

        /* [ コマンドのネスト回数をここでセット */
        size_t nestnum_pos = ls->loop_start - 1;
        c->out[nestnum_pos] = count;

        /* [ の命令位置に飛ぶオフセットを算出するのに ] の位置を保持 */
        int32_t jump_pos = c->out_len;
        /* オフセットの飛び先は保存した loop_start */
        int32_t offset = (int32_t)ls->loop_start - (jump_pos + 3);
        if (offset >= -256 && offset <= -1) {
            /* 1 バイトオフセット（FFxx パターン） */
            /* オフセットが1バイトなので飛び先も1バイトずらす */
            offset++;
            uint8_t off8 = (uint8_t)(offset & 0xFF);
            emit_byte(c, 0xF1);
            emit_byte(c, off8);
        } else {
            /* 2バイトオフセット */
            uint16_t off16 = (uint16_t)offset;
            emit_byte(c, 0xF2);
            emit_word_le(c, off16);
        }

        /* : があれば、その 2byte に exit offset を書く */
        if (ls->exit_mark != LOOP_NOEXIT) {
            int32_t jump_pos = c->out_len;
            size_t colon_pos = ls->exit_mark - 3; /* ':'コマンド長=3 */
            /* : の次の 2byte に offset を書く */
            int32_t ex_off = (int32_t)jump_pos - (int32_t)(colon_pos + 3);
            uint16_t ex16 = (uint16_t)ex_off;
            c->out[colon_pos + 1] = (uint8_t)(ex16 & 0xFF);
            c->out[colon_pos + 2] = (uint8_t)(ex16 >> 8);
        }

        c->nest_depth--;
        if (ls->saved_l_len96 != 0) {
            c->l_len96 = ls->saved_l_len96;
            ls->saved_l_len96 = 0;
        }
        if (ls->saved_lp_len96 != 0) {
            c->lp_len96 = ls->saved_lp_len96;
            ls->saved_lp_len96 = 0;
        }
        if (ls->saved_octave != 0) {
            c->octave = ls->saved_octave;
            ls->saved_octave = 0;
            c->octave_last = ls->saved_octave_last;
            ls->saved_octave_last = 0;
        }
        break;
    }
    case ':': { /* ネスト脱出 */
        if (c->nest_depth <= 0) {
            set_error(c, MML_ERR_OUT_OF_NEST,
              "':'コマンドをネスト'[',']'の外で使用しています");
            /* 解析上はこのあとのネスト終了をパースできないので一旦リセット */
            c->nest_depth = 0;
            return;
        }
        MML_LoopState *ls = &c->loops[c->nest_depth - 1];
        if (ls->exit_mark != LOOP_NOEXIT) {
            set_error(c, MML_ERR_DUP_EXIT,
              "':'コマンドをネスト'[',']'の中で複数指定しています");
            /* 解析上はこのあとのネスト終了をパースできないので一旦リセット */
            c->nest_depth = 0;
            return;
        }
        emit_byte(c, 0xF3);
        emit_word_le(c, 0x0000); /* 後で ] 側で埋める */
        ls->exit_mark = c->out_len;
        ls->saved_l_len96 = c->l_len96;
        ls->saved_lp_len96 = c->lp_len96;
        ls->saved_octave = (uint8_t)c->octave;
        ls->saved_octave_last = (uint8_t)c->octave_last;
        break;
    }
    case ';': { /* コメント */
        /* 残り行データをすべて読み捨てて return */
        while ((ch = peek(c)) >= 0 && ch != '\n')
            (void)get(c);
        break;
    }
    default:
        set_error(c, MML_ERR_SYNTAX,
          "MML仕様にない数字や文字が使用されています");
        return;
    }
}
