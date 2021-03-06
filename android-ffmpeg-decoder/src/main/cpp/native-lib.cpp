extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"

#include <unistd.h>
#include <fcntl.h>
#include <android/log.h>
#include <sys/stat.h>
}

#include <string>
#include <map>

#define LOGE(format, ...) __android_log_print(ANDROID_LOG_ERROR, "(>_<)", format, ##__VA_ARGS__);
#define LOGI(format, ...) __android_log_print(ANDROID_LOG_INFO, "(>_<)", format, ##__VA_ARGS__);


#include "com_itdog_decoder_NativeBridge.h"

void custom_log(void *ptr, int level, const char *fmt, va_list vl);

/*
 * Class:     com_itdog_decoder_NativeBridge
 * Method:    getVersion
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_com_itdog_decoder_NativeBridge_getVersion
        (JNIEnv *env, jclass thiz) {
    int version = 5;
    LOGI("getVersion: %d.", version);
    return version;
}

/**
 * 文件是否存在
 * @param file_path
 * @return
 */
static bool FileExists(const char *file_path) {
    return access(file_path, 0) == 0;
}

/**
 * 初始化
 */
static void InitFFmpeg() {
    av_log_set_callback(custom_log);
    av_register_all();
    avformat_network_init();
}

/**
 * 获取格式化后的视频长度
 * @param avFormatContext
 * @param buf
 * @param len
 */
static void GetVideoLengthStringFormat(AVFormatContext *avFormatContext, char *buf, size_t len) {

    if (avFormatContext != NULL && buf != NULL && len > 0) {

        int64_t duration = avFormatContext->duration;
        int tns, thh, tmm, tss;
        tns = duration / 1000000;
        thh = tns / 3600;
        tmm = (tns % 3600) / 60;
        tss = tns % 60;
        snprintf(buf, len, "%02d:%02d:%02d", thh, tmm, tss);

    }

}

/**
 * 读取视频元信息
 * @param pAvFormatContext
 * @return 视频元信息集合
 */
static std::map<std::string, std::string> GetVideoMetaData(AVFormatContext *pAvFormatContext) {

    std::map<std::string, std::string> result;
    AVDictionaryEntry *entry = NULL;

    if (pAvFormatContext != NULL) {

        while ((entry = av_dict_get(pAvFormatContext->metadata, "", entry,
                                    AV_DICT_IGNORE_SUFFIX))) {

            result.insert(std::pair<std::string, std::string>(entry->key, entry->value));

        }
    }

    return result;
}


/*
 * Class:     com_itdog_decoder_NativeBridge
 * Method:    decode
 * Signature: (Ljava/lang/String;Ljava/lang/String;)I
 */
JNIEXPORT jint JNICALL Java_com_itdog_decoder_NativeBridge_decode
        (JNIEnv *env, jclass thiz, jstring input, jstring output) {
    AVFormatContext *avFormatContext;
    int i, videoIndex;
    AVCodecContext *avCodecContext;
    AVCodec *avCodec;
    AVFrame *avFrame, *avFrameYUV;
    uint8_t *outBuf;
    AVPacket *avPacket;
    int ySize;
    int ret, gotPicture;
    struct SwsContext *imgConvertContext;
    int fpYUV;
    int frameCount;
    clock_t timeStart, timeFinish;
    double timeDuration;
    char inputStr[500] = {0};
    char outputStr[500] = {0};
    char info[1000] = {0};

    InitFFmpeg();

    sprintf(inputStr, "%s", env->GetStringUTFChars(input, NULL));
    sprintf(outputStr, "%s", env->GetStringUTFChars(output, NULL));


    avFormatContext = avformat_alloc_context();

    if (FileExists(inputStr)) {
        LOGI("file %s exists.", inputStr);
    } else {
        LOGI("file %s not exists.", inputStr);
    }

    if (avformat_open_input(&avFormatContext, inputStr, NULL, NULL) != 0) {
        LOGE("couldn't open input stream!.\n");
        return -1;
    }

    // 打印视频时长
    char videoLength[256];
    bzero(videoLength, sizeof(videoLength));
    GetVideoLengthStringFormat(avFormatContext, videoLength, sizeof(videoLength));
    LOGI("video length: %s.\n", videoLength);

    if (avformat_find_stream_info(avFormatContext, NULL) < 0) {
        LOGE("couldn't find stream information!.\n");
        return -1;
    }

    // 打印视频元信息
    std::map<std::string, std::string> metaData = GetVideoMetaData(avFormatContext);
    LOGI("meta data count: %d.", metaData.size());
    for (auto i = metaData.begin(); i != metaData.end(); i++) {
        LOGI("meta data[%s] = %s.\n", i->first.c_str(), i->second.c_str());
    }

    videoIndex = -1;
    for (i = 0; i < avFormatContext->nb_streams; i++) {
        if (avFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIndex = i;
            break;
        }
    }

    if (videoIndex == -1) {
        LOGE("couldn't find a video stream.\n");
        return -1;
    }

    avCodecContext = avFormatContext->streams[videoIndex]->codec;
    avCodec = avcodec_find_decoder(avCodecContext->codec_id);
    if (avCodec == NULL) {
        LOGE("couldn't find codec.\n");
        return -1;
    }

    if (avcodec_open2(avCodecContext, avCodec, NULL) < 0) {
        LOGE("couldn't open codec.\n");
        return -1;
    }

    avFrame = av_frame_alloc();
    avFrameYUV = av_frame_alloc();
    outBuf = (uint8_t *) av_malloc(
            av_image_get_buffer_size(AV_PIX_FMT_YUV420P, avCodecContext->width,
                                     avCodecContext->height, 1));
    av_image_fill_arrays(avFrameYUV->data, avFrameYUV->linesize, outBuf, AV_PIX_FMT_YUV420P,
                         avCodecContext->width, avCodecContext->height, 1);
    avPacket = (AVPacket *) av_malloc(sizeof(AVPacket));
    imgConvertContext = sws_getContext(avCodecContext->width, avCodecContext->height,
                                       avCodecContext->pix_fmt,
                                       avCodecContext->width, avCodecContext->height,
                                       AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

    sprintf(info, "[Input ]%s\n", inputStr);
    sprintf(info, "%s[Output ]%s\n", info, outputStr);
    sprintf(info, "%s[Format ]%s\n", info, avFormatContext->iformat->name);
    sprintf(info, "%s[Codec ]%s\n", info, avCodecContext->codec->name);
    sprintf(info, "%s[Resolution ]%dx%d\n", info, avCodecContext->width, avCodecContext->height);

    fpYUV = open(outputStr, O_CREAT | O_WRONLY);
    if (fpYUV <= 0) {
        LOGE("couldn't open output file.\n");
        return -1;
    }

    frameCount = 0;
    timeStart = clock();
    while (av_read_frame(avFormatContext, avPacket) >= 0) {

        if (avPacket->stream_index == videoIndex) {

            ret = avcodec_decode_video2(avCodecContext, avFrame, &gotPicture, avPacket);
            if (ret < 0) {
                LOGE("decode error.\n");
                return -1;
            }

            if (gotPicture) {

                sws_scale(imgConvertContext, (const uint8_t *const *) avFrame->data,
                          avFrame->linesize, 0,
                          avCodecContext->height, avFrameYUV->data, avFrameYUV->linesize);
                ySize = avCodecContext->width * avCodecContext->height;
                write(fpYUV, avFrameYUV->data[0], ySize);
                write(fpYUV, avFrameYUV->data[1], ySize / 4);
                write(fpYUV, avFrameYUV->data[2], ySize / 4);

                char pictureType[10] = {0};
                bzero(pictureType, sizeof(pictureType));
                switch (avFrame->pict_type) {
                    case AV_PICTURE_TYPE_I:
                        snprintf(pictureType, sizeof(pictureType), "I");
                        break;
                    case AV_PICTURE_TYPE_P:
                        snprintf(pictureType, sizeof(pictureType), "P");
                        break;
                    case AV_PICTURE_TYPE_B:
                        snprintf(pictureType, sizeof(pictureType), "B");
                        break;
                    default:
                        snprintf(pictureType, sizeof(pictureType), "OTHER");
                        break;
                }
                //LOGI("Frame index: %5d, type: %s ysize:%d", frameCount, pictureType, ySize);

                frameCount++;
            }

            av_free_packet(avPacket);
        }
    }

    while (1) {

        ret = avcodec_decode_video2(avCodecContext, avFrame, &gotPicture, avPacket);
        if (ret < 0) {
            LOGI("avcodec_decode_video2 finished.\n");
            break;
        }

        if (!gotPicture) {
            LOGI("avcodec_decode_video2 gotPicture = 0.\n");
            break;
        }

        sws_scale(imgConvertContext, (const uint8_t *const *) avFrame->data, avFrame->linesize, 0,
                  avFrame->height,
                  avFrameYUV->data, avFrameYUV->linesize);
        int ySize = avCodecContext->width * avCodecContext->height;
        write(fpYUV, avFrameYUV->data[0], ySize);
        write(fpYUV, avFrameYUV->data[1], ySize / 4);
        write(fpYUV, avFrameYUV->data[2], ySize / 4);

        char pictureType[10] = {0};
        bzero(pictureType, sizeof(pictureType));
        switch (avFrame->pict_type) {
            case AV_PICTURE_TYPE_I:
                snprintf(pictureType, sizeof(pictureType), "I");
                break;
            case AV_PICTURE_TYPE_P:
                snprintf(pictureType, sizeof(pictureType), "P");
                break;
            case AV_PICTURE_TYPE_B:
                snprintf(pictureType, sizeof(pictureType), "B");
                break;
            default:
                snprintf(pictureType, sizeof(pictureType), "OTHER");
                break;
        }
        //LOGI("Frame index: %5d, type: %s", frameCount, pictureType);

        frameCount++;
    }

    timeFinish = clock();
    timeDuration = timeFinish - timeStart;

    sprintf(info, "%s[Time ]%fms\n", info, timeDuration);
    sprintf(info, "%s[Count ]%d\n", info, frameCount);

    LOGI("%s", info);

    sws_freeContext(imgConvertContext);
    close(fpYUV);
    av_frame_free(&avFrameYUV);
    av_frame_free(&avFrame);
    avcodec_close(avCodecContext);
    avformat_close_input(&avFormatContext);

    return 0;
}


void custom_log(void *ptr, int level, const char *fmt, va_list vl) {
    FILE *fp = fopen("/storage/emulated/0/av_log.txt", "a+");
    if (fp) {
        vfprintf(fp, fmt, vl);
        fflush(fp);
        fclose(fp);
    }
}


