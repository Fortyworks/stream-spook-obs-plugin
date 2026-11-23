/*
 * stb_image_write の実体をここで 1 回だけ組む（program-frames.c が JPEG に使う）。
 * stb_image_write.h は https://github.com/nothings/stb の v1.18 をそのまま写したもの。
 * public domain / MIT のどちらかを選べる（ライセンス本文は stb_image_write.h の末尾）。
 * ファイルへ書く口は使わないので外す。
 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
