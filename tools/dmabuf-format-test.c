/* Offline regression tests: no Portal, PipeWire connection or screen capture. */
#include "../src/qq-wl-portal.c"
#include <spa/pod/filter.h>

static unsigned checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static struct spa_pod *qq_format(struct spa_pod_builder *b)
{
    return spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(6, SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_ABGR_210LE, SPA_VIDEO_FORMAT_xBGR_210LE),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(320, 240), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
}

static struct spa_pod *niri_format(struct spa_pod_builder *b)
{
    struct spa_pod_frame object, choice;
    spa_pod_builder_push_object(b, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx), 0);
    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
        SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
    const int64_t mods[] = {0, 0, INT64_C(0x0100000000000001),
        INT64_C(0x0100000000000002), INT64_C(0x0100000000000006),
        INT64_C(0x0100000000000008), INT64_C(0x00ffffffffffffff)};
    for (size_t i = 0; i < SPA_N_ELEMENTS(mods); ++i)
        spa_pod_builder_long(b, mods[i]);
    spa_pod_builder_pop(b, &choice);
    spa_pod_builder_add(b,
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(2560, 1440)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(165000, 1000), &SPA_FRACTION(1, 1),
            &SPA_FRACTION(165000, 1000)), 0);
    return spa_pod_builder_pop(b, &object);
}

static int filter(const struct spa_pod *a, const struct spa_pod *b)
{
    uint8_t storage[4096];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    return spa_pod_filter(&builder, NULL, a, b);
}

static void test_formats(void)
{
    uint8_t a[2048], saved[2048], b[2048], extra[2048];
    struct spa_pod_builder ab = SPA_POD_BUILDER_INIT(a, sizeof(a));
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(b, sizeof(b));
    struct spa_pod *qq = qq_format(&ab), *niri = niri_format(&bb);
    memcpy(saved, qq, SPA_POD_SIZE(qq));
    CHECK(filter(qq, niri) == -EINVAL);
    struct spa_pod *dma = linear_dmabuf_format(qq);
    CHECK(dma != NULL);
    CHECK(filter(dma, niri) == 0);
    CHECK(filter(niri, dma) == 0);
    uint8_t negotiated[4096];
    struct spa_pod_builder nb = SPA_POD_BUILDER_INIT(negotiated, sizeof(negotiated));
    struct spa_pod *format_pod = NULL;
    CHECK(spa_pod_filter(&nb, &format_pod, dma, niri) == 0);
    CHECK(spa_pod_fixate(format_pod) == 0);
    struct stream_info state = { .dmabuf_linear = 1 };
    on_param_changed(&state, SPA_PARAM_Format, format_pod);
    CHECK(state.dmabuf_ready && state.dmabuf_invalidate);
    CHECK(state.width == 2560 && state.height == 1440);
    CHECK(memcmp(saved, qq, SPA_POD_SIZE(qq)) == 0);
    /* 协商成共享内存时不算 DMA-BUF 就绪。 */
    uint8_t shm_buf[2048], shm_neg[4096];
    struct spa_pod_builder sb = SPA_POD_BUILDER_INIT(shm_buf, sizeof(shm_buf));
    struct spa_pod *shm = spa_pod_builder_add_object(&sb, SPA_TYPE_OBJECT_Format,
        SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(1920, 1080)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)));
    struct spa_pod_builder snb = SPA_POD_BUILDER_INIT(shm_neg, sizeof(shm_neg));
    struct spa_pod *shm_format = NULL;
    CHECK(spa_pod_filter(&snb, &shm_format, qq, shm) == 0);
    CHECK(spa_pod_fixate(shm_format) == 0);
    struct stream_info shm_state = { .dmabuf_linear = 1, .dmabuf_ready = 1 };
    on_param_changed(&shm_state, SPA_PARAM_Format, shm_format);
    CHECK(!shm_state.dmabuf_ready && shm_state.width == 1920);
    const struct spa_pod_prop *p = spa_pod_find_prop(dma, NULL, SPA_FORMAT_VIDEO_modifier);
    int64_t modifier = -1;
    CHECK(p && (p->flags & SPA_POD_PROP_FLAG_MANDATORY));
    CHECK(!(p->flags & SPA_POD_PROP_FLAG_DONT_FIXATE));
    CHECK(spa_pod_get_long(&p->value, &modifier) == 0 && modifier == 0);
    uint32_t format = 0;
    p = spa_pod_find_prop(dma, NULL, SPA_FORMAT_VIDEO_format);
    CHECK(p && spa_pod_get_id(&p->value, &format) == 0 && format == SPA_VIDEO_FORMAT_BGRx);
    const uint32_t preserved[] = { SPA_FORMAT_VIDEO_size, SPA_FORMAT_VIDEO_framerate };
    for (size_t i = 0; i < SPA_N_ELEMENTS(preserved); ++i) {
        p = spa_pod_find_prop(qq, NULL, preserved[i]);
        const struct spa_pod_prop *q = spa_pod_find_prop(dma, NULL, preserved[i]);
        CHECK(p && q && spa_pod_memcmp(&p->value, &q->value) == 0);
    }
    CHECK(linear_dmabuf_format(dma) == NULL);
    CHECK(linear_dmabuf_format(NULL) == NULL);
    struct spa_pod_builder eb = SPA_POD_BUILDER_INIT(extra, sizeof(extra));
    struct spa_pod *other = spa_pod_builder_add_object(&eb, SPA_TYPE_OBJECT_Format,
        SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw));
    CHECK(linear_dmabuf_format(other) == NULL);
    eb = SPA_POD_BUILDER_INIT(extra, sizeof(extra));
    other = spa_pod_builder_add_object(&eb, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_RGBA));
    CHECK(linear_dmabuf_format(other) == NULL);
    free(dma);
}

static struct pw_buffer *next_buffer;
static unsigned queued;
static struct pw_buffer *mock_dequeue(struct pw_stream *stream)
{
    (void)stream;
    return next_buffer;
}
static int mock_queue(struct pw_stream *stream, struct pw_buffer *buffer)
{
    (void)stream;
    CHECK(buffer == next_buffer);
    ++queued;
    return 0;
}

static void test_buffers(void)
{
    uint8_t capture[0x230] = {0};
    struct spa_chunk chunk = { .stride = 10240, .offset = 128 };
    struct spa_data planes[2] = {{ .type = SPA_DATA_DmaBuf, .fd = 17,
        .maxsize = 1, .chunk = &chunk }};
    struct spa_buffer spa = { .n_datas = 1, .datas = planes };
    struct pw_buffer buffer = { .buffer = &spa };
    struct stream_info stream = { .stream = (void *)&buffer, .width = 2560,
        .height = 1440, .dmabuf_capture = capture, .dmabuf_linear = 1,
        .dmabuf_ready = 1, .dmabuf_invalidate = 1 };
    streams = &stream;
    next_buffer = &buffer;
    real_dequeue = mock_dequeue;
    real_queue = mock_queue;
    CHECK(wrap_dequeue(stream.stream) == &buffer);
    CHECK(queued == 0);
    CHECK(*(int32_t *)(capture + 0x1f4) == -1);
    CHECK(stream.dmabuf_invalidate == 0);
    spa.n_datas = 2;
    CHECK(wrap_dequeue(stream.stream) == NULL && queued == 1);
    spa.n_datas = 1;
    chunk.stride = 1;
    CHECK(wrap_dequeue(stream.stream) == NULL && queued == 2);
    chunk.stride = 10240;
    stream.dmabuf_ready = 0;
    CHECK(wrap_dequeue(stream.stream) == NULL && queued == 3);
    stream.dmabuf_linear = 0;
    CHECK(wrap_dequeue(stream.stream) == &buffer && queued == 3);
    stream.dmabuf_linear = 1;
    planes[0].type = SPA_DATA_MemPtr;
    planes[0].data = capture;
    chunk.offset = 0;
    CHECK(wrap_dequeue(stream.stream) == &buffer && queued == 3);
    streams = NULL;
}

int main(void)
{
    test_formats();
    test_buffers();
    CHECK(has_egl_extension(NULL, "EGL_EXT_image_dma_buf_import") == 0);
    CHECK(has_egl_extension("EGL_EXT_image_dma_buf_import_modifiers",
                            "EGL_EXT_image_dma_buf_import") == 0);
    CHECK(has_egl_extension("other EGL_EXT_image_dma_buf_import next",
                            "EGL_EXT_image_dma_buf_import") == 1);
    printf("PASS: %u checks; recorded -22 rejected, LINEAR DMA-BUF accepted\n", checks);
    return 0;
}
