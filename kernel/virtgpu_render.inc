/* VirGL BLIT deliberately does not implement source-over on some renderers.
 * Alpha layers therefore use an explicit textured-quad shader and blend state.
 * VertexID generates the quad; the kernel needs neither vertex buffers nor FPU.
 */
static bool quad_ready;
static u32 stream_words;

static u32 *emit(u32 op, u32 object, u32 words) {
    u32 *p = (void *)(command + 32 + stream_words * 4);
    memset(p, 0, (words + 1) * 4);
    p[0] = op | (object << 8) | (words << 16);
    stream_words += words + 1;
    return p;
}
static bool submit_stream(void) {
    ((u32 *)command)[6] = stream_words * 4;
    ((u32 *)command)[7] = 0;
    return send(32 + stream_words * 4, 0x207, CTX);
}
static void shader(u32 handle, u32 type, const char *text) {
    u32 bytes = (strlen(text) + 1 + 3) & ~3u;
    u32 *p = emit(1, 4, 5 + bytes / 4);
    p[1] = handle; p[2] = type; p[3] = bytes; p[4] = 1024;
    memcpy(p + 6, text, strlen(text) + 1);
}
static void bind(u32 object, u32 handle) { emit(2, object, 1)[1] = handle; }

static bool init_quad(void) {
    static const char vertex[] =
        "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..1]\n"
        "DCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL TEMP[0]\n"
        "IMM[0] UINT32 {1, 2, 0, 0}\nIMM[1] FLT32 {0, 1, 0, 0}\n"
        "AND TEMP[0].xy, SV[0].xxxx, IMM[0].xyxx\n"
        "UCMP OUT[0].x, TEMP[0].xxxx, CONST[0].zzzz, CONST[0].xxxx\n"
        "UCMP OUT[0].y, TEMP[0].yyyy, CONST[0].wwww, CONST[0].yyyy\n"
        "MOV OUT[0].z, IMM[1].xxxx\nMOV OUT[0].w, IMM[1].yyyy\n"
        "UCMP OUT[1].x, TEMP[0].xxxx, CONST[1].zzzz, CONST[1].xxxx\n"
        "UCMP OUT[1].y, TEMP[0].yyyy, CONST[1].wwww, CONST[1].yyyy\n"
        "MOV OUT[1].zw, IMM[1].xxxx\nEND\n";
    static const char fragment[] =
        "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n"
        "DCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\nDCL CONST[0]\nDCL TEMP[0]\n"
        "TEX TEMP[0], IN[0], SAMP[0], 2D\nMOV OUT[0], TEMP[0]\n"
        "MUL OUT[0].w, TEMP[0].wwww, CONST[0].xxxx\nEND\n";
    stream_words = 0;
    u32 *p = emit(1, 1, 11); p[1] = 100;
    /* straight-alpha source-over, independent alpha equation, all channels */
    p[4] = 1 | (3u << 4) | (19u << 9) | (1u << 17) | (19u << 22) | (15u << 27);
    p = emit(1, 2, 9); p[1] = 101; p[2] = 2 | (1u << 29); p[3] = p[6] = 0x3f800000;
    p = emit(1, 3, 5); p[1] = 102;
    p = emit(1, 5, 1); p[1] = 103;
    shader(104, 0, vertex); shader(105, 1, fragment);
    p = emit(1, 7, 9); p[1] = 106; p[2] = 2 | (2u << 3) | (2u << 6) | (2u << 11);
    bind(1, 100); bind(2, 101); bind(3, 102); bind(5, 103);
    p = emit(31, 0, 2); p[1] = 104; p[2] = 0;
    p = emit(31, 0, 2); p[1] = 105; p[2] = 1;
    p = emit(18, 0, 3); p[1] = 1; p[3] = 106;
    quad_ready = submit_stream();
    return quad_ready;
}

/* Integer conversion truncates by less than one float ULP.
 * Keeping FP out of ring 0 avoids corrupting an application's FPU state. */
static u32 f32_ratio(int numerator, u32 denominator) {
    if (!numerator) return 0;
    u32 sign = numerator < 0 ? 0x80000000u : 0;
    u32 n = numerator < 0 ? -numerator : numerator, d = denominator;
    int exponent = 0;
    while (n < d) { n <<= 1; exponent--; }
    while (n >= d * 2) { d <<= 1; exponent++; }
    u32 rem = n - d, mantissa = 0;
    for (int i = 0; i < 23; i++) {
        rem <<= 1; mantissa <<= 1;
        if (rem >= d) { rem -= d; mantissa |= 1; }
    }
    return sign | ((u32)(exponent + 127) << 23) | mantissa;
}

static bool alpha_blit(u32 src, u32 sw, u32 sh, int sx, int sy, int dx, int dy, int w, int h,u32 format,u32 opacity) {
    if (!quad_ready && !init_quad()) return false;
    stream_words = 0;
    u32 *p = emit(1, 8, 5); p[1] = 108; p[2] = back; p[3] = FORMAT;
    p = emit(5, 0, 3); p[1] = 1; p[3] = 108;
    p = emit(4, 0, 7);
    p[2] = p[5] = f32_ratio(width, 2); p[3] = p[6] = f32_ratio(height, 2);
    p[4] = p[7] = 0x3f000000; /* Z scale/translate */
    p = emit(1, 6, 6); p[1] = 107; p[2] = src; p[3] = format | (2u << 24);
    p[6] = 0 | (1u << 3) | (2u << 6) | (3u << 9); /* identity RGBA swizzle */
    p = emit(10, 0, 3); p[1] = 1; p[3] = 107;
    p = emit(12, 0, 10); /* VS constant slot 0: clip bounds, then UV bounds */
    p[3] = f32_ratio(2 * dx - (int)width, width);
    p[4] = f32_ratio((int)height - 2 * dy, height);
    p[5] = f32_ratio(2 * (dx + w) - (int)width, width);
    p[6] = f32_ratio((int)height - 2 * (dy + h), height);
    p[7] = f32_ratio(sx, sw); p[8] = f32_ratio(sh - sy, sh);
    p[9] = f32_ratio(sx + w, sw); p[10] = f32_ratio(sh - sy - h, sh);
    p = emit(12,0,6); p[1]=1; p[3]=f32_ratio(opacity,255); /* fragment constant slot 0 */
    p = emit(8, 0, 12); p[2] = 4; p[3] = 5; p[5] = 1; p[11] = 3; /* triangle strip */
    emit(3, 6, 1)[1] = 107;
    emit(3, 8, 1)[1] = 108;
    return submit_stream();
}
