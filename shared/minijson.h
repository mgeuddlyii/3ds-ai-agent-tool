/*
 * minijson.h -- 极小的 JSON 遍历器（回调式，无动态分配）
 *
 * 为什么不直接用 jsmn：jsmn 产出的是 token 数组，调用方还要自己按嵌套
 * 关系拼路径；而本项目读 JSON 只有一件事 —— "把 security.current_level
 * 这个值拿出来"。所以直接让解析器在走到每个标量时回调一次，并把**点分
 * 路径**一并给出，调用方一个 strcmp 链就完事了。
 *
 * 支持：对象、数组、字符串（含 \" \\ \/ \b \f \n \r \t \uXXXX 转 UTF-8）、
 *       数字（整数/小数/指数）、true/false/null。注释不支持（真 JSON 也没有）。
 *
 * 不做的事：不建 DOM、不分配内存、不报行号（报字节偏移就够定位了）。
 *
 * 回调契约：v->str 指向解析器内部的暂存缓冲，**只在回调期间有效**，
 * 需要留存请自己拷走。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MJ_NULL = 0,
    MJ_BOOL,
    MJ_NUMBER,
    MJ_STRING,
    MJ_ARRAY_START,
    MJ_ARRAY_END,
    MJ_OBJECT_START,
    MJ_OBJECT_END,
    MJ_ERROR,
} MiniJsonType;

typedef struct {
    MiniJsonType type;
    const char  *path;     /* 点分路径，如 "security.current_level"。
                            * 数组元素**不**拼 [i]：路径仍是 "artifacts.kind"，
                            * 下标看 index 字段。 */
    const char  *str;      /* MJ_STRING 专用，见回调契约 */
    double       num;      /* MJ_NUMBER / MJ_BOOL（true=1） */
    bool         boolean;  /* MJ_BOOL 专用 */
    int          index;    /* 若该标量位于数组内，为其下标；否则 -1 */
} MiniJsonValue;

/* 返回 false 会中止整个解析（调用方可以用来"只要第一个命中项"）。 */
typedef bool (*MiniJsonFn)(const MiniJsonValue *v, void *user);

/* 解析 text[0..len)。数字用内置的有界十进制解析（不依赖 strtod），
 * 不要求 text 以 '\0' 结尾；允许开头有 UTF-8 BOM，不允许尾部有非空白残留。 */
bool minijson_walk(const char *text, size_t len, MiniJsonFn fn, void *user);

const char *minijson_type_name(MiniJsonType t);
