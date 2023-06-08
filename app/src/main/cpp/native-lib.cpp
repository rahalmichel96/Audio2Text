#include <jni.h>
#include <cstdlib>
#include <cstring>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

extern "C" {
    #include <libavutil/opt.h>
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswresample/swresample.h>
}

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cstdio>
#include <android/log.h>
#include "tensorflow/lite/core/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/optional_debug_tools.h"
#include "whisper.h"
#include "input_features.h"
#include "tensorflow/lite/delegates/gpu/delegate.h"
#include <fstream>
#include <memory>

#define INFERENCE_ON_AUDIO_FILE 1

#define TFLITE_MINIMAL_CHECK(x)                              \
  if (!(x)) {                                                \
    fprintf(stderr, "Error at %s:%d\n", __FILE__, __LINE__); \
    exit(1);                                                 \
  }

#define WAVE_SAMPLE_RATE        16000
#define AVIO_CTX_BUF_SZ          4096
#define TARGET_CHANNELS 1
#define TARGET_SAMPLE_RATE 16000
#define TARGET_SAMPLE_FORMAT AV_SAMPLE_FMT_S16
#define TAG "ffmpeg_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct wave_hdr {
    /* RIFF Header: "RIFF" */
    char riff_header[4];
    /* size of audio data + sizeof(struct wave_hdr) - 8 */
    int wav_size;
    /* "WAVE" */
    char wav_header[4];
    /* Format Header */
    /* "fmt " (includes trailing space) */
    char fmt_header[4];
    /* Should be 16 for PCM */
    int fmt_chunk_size;
    /* Should be 1 for PCM. 3 for IEEE Float */
    int16_t audio_format;
    int16_t num_channels;
    int sample_rate;
    /*
    * Number of bytes per second
    * sample_rate * num_channels * bit_depth/8
    */
    int byte_rate;
    /* num_channels * bytes per sample */
    int16_t sample_alignment;
    /* bits per sample */
    int16_t bit_depth;

    /* Data Header */
    /* "data" */
    char data_header[4];
    /*
    * size of audio
    * number of samples * num_channels * bit_depth/8
    */
    int data_bytes;
} __attribute__((__packed__));

struct audio_buffer {
    uint8_t *ptr;
    int size; /* size left in the buffer */
};

static void write_wave_hdr(int fd, size_t size)
{
    struct wave_hdr wh;

    memcpy(&wh.riff_header, "RIFF", 4);
    wh.wav_size = size + sizeof(struct wave_hdr) - 8;
    memcpy(&wh.wav_header, "WAVE", 4);
    memcpy(&wh.fmt_header, "fmt ", 4);
    wh.fmt_chunk_size = 16;
    wh.audio_format = 1;
    wh.num_channels = 1;
    wh.sample_rate = WAVE_SAMPLE_RATE;
    wh.sample_alignment = 2;
    wh.bit_depth = 16;
    wh.byte_rate = wh.sample_rate * wh.sample_alignment;
    memcpy(&wh.data_header, "data", 4);
    wh.data_bytes = size;

    write(fd, &wh, sizeof(struct wave_hdr));
}

AVFormatContext* openSourceFile(const char* sourceFilePath) {
    AVFormatContext* formatContext = nullptr;

    // Ouvrir le fichier source
    int result = avformat_open_input(&formatContext, sourceFilePath, nullptr, nullptr);
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to open source file: %s", av_err2str(result));
        return nullptr;
    }

    // Récupérer les informations sur les flux dans le fichier source
    result = avformat_find_stream_info(formatContext, nullptr);
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to retrieve source stream info: %s", av_err2str(result));
        avformat_close_input(&formatContext);
        return nullptr;
    }

    return formatContext;
}

int findAudioStreamIndex(AVFormatContext* formatContext) {
    // Trouver l'index du flux audio
    int audioStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to find an audio stream in the source file");
        return -1;
    }

    return audioStreamIndex;
}

AVCodecContext* initializeAudioDecoder(AVFormatContext* formatContext, int audioStreamIndex) {
    // Récupérer le codec paramètres pour le flux audio
    AVCodecParameters* codecParameters = formatContext->streams[audioStreamIndex]->codecpar;

    // Trouver le décodeur pour le codec
    auto* codec = const_cast<AVCodec *>(avcodec_find_decoder(codecParameters->codec_id));
    if (!codec) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to find decoder for audio stream");
        return nullptr;
    }

    // Allouer un contexte de codec et l'initialiser avec les paramètres du codec
    AVCodecContext* codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to allocate codec context");
        return nullptr;
    }

    // Initialiser le contexte du codec avec les paramètres du codec
    int result = avcodec_parameters_to_context(codecContext, codecParameters);
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to initialize codec context: %s", av_err2str(result));
        avcodec_free_context(&codecContext);
        return nullptr;
    }

    // Ouvrir le codec
    result = avcodec_open2(codecContext, codec, nullptr);
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to open codec: %s", av_err2str(result));
        avcodec_free_context(&codecContext);
        return nullptr;
    }

    return codecContext;
}

std::vector<AVFrame*> decodeAudioPackets(AVFormatContext* formatContext, int audioStreamIndex, AVCodecContext* codecContext) {
    std::vector<AVFrame*> frames;

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to allocate packet");
        return frames;
    }

    while (av_read_frame(formatContext, packet) >= 0) {
        // Ignorer les paquets qui ne proviennent pas du flux audio
        if (packet->stream_index != audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        // Envoyer le paquet au décodeur
        int result = avcodec_send_packet(codecContext, packet);
        if (result < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to send packet to decoder: %s", av_err2str(result));
            av_packet_unref(packet);
            continue;
        }

        // Recevoir tous les frames du paquet
        while (result >= 0) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to allocate frame");
                break;
            }

            result = avcodec_receive_frame(codecContext, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                av_frame_free(&frame);
                break;
            } else if (result < 0) {
                __android_log_print(ANDROID_LOG_ERROR, "Audio Conversion", "Failed to receive frame from decoder: %s", av_err2str(result));
                av_frame_free(&frame);
                break;
            }

            frames.push_back(frame);
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    return frames;
}

static void convert_frame(struct SwrContext *swr, AVCodecContext *codec,
                          AVFrame *frame, int16_t **data, int *size, bool flush)
{
    int nr_samples;
    int64_t delay;
    uint8_t *buffer;

    delay = swr_get_delay(swr, codec->sample_rate);
    nr_samples = av_rescale_rnd(delay + (!flush ? frame->nb_samples : 0),
                                WAVE_SAMPLE_RATE, codec->sample_rate,
                                AV_ROUND_UP);
    av_samples_alloc(&buffer, NULL, 1, nr_samples, AV_SAMPLE_FMT_S16, 0);

    /*
     * !flush is used to check if we are flushing any remaining
     * conversion buffers...
     */
    nr_samples = swr_convert(swr, &buffer, nr_samples,
                             !flush ? (const uint8_t **)frame->data : NULL,
                             !flush ? frame->nb_samples : 0);

    *data = static_cast<int16_t *>(realloc(*data, (*size + nr_samples) * sizeof(int16_t)));
    memcpy(*data + *size, buffer, nr_samples * sizeof(int16_t));
    *size += nr_samples;
    av_freep(&buffer);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_audio2text_MainActivity_freeModelJNI(
        JNIEnv* env,
        jobject /* this */) {
    if(g_whisper_tflite_params.buffer){
        __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR",
                            "%s: free buffer %x memory\n", __func__,
                            g_whisper_tflite_params.buffer);
        free(g_whisper_tflite_params.buffer);

    }
    return 0;
}

extern "C" JNIEXPORT jint JNICALL Java_com_example_audio2text_MainActivity_convertTo16kHz(JNIEnv* env, jobject thiz, jstring inputFilePath, jstring outputFilePath) {
    const char* inputPath = env->GetStringUTFChars(inputFilePath, nullptr);
    const char* outputPath = env->GetStringUTFChars(outputFilePath, nullptr);

    AVFormatContext *formatContext = nullptr;
    int audioStreamIndex = -1;
    AVCodecContext *codecContext = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swrContext = nullptr;
    int fd;
    size_t size;
    uint8_t *ptr;
    int16_t *data = nullptr;
    int dataSize = 0;
    int ret;

    formatContext = openSourceFile(inputPath);
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi opensourcefile");
    audioStreamIndex = findAudioStreamIndex(formatContext);
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi findaudiostreamindex");
    codecContext = initializeAudioDecoder(formatContext, audioStreamIndex);
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi initializeaudioDecoder");

    frame = av_frame_alloc();
    if (!frame) {
        LOGE("Failed to allocate frame");
        return -1;
    }
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi allocate frame");

    swrContext = swr_alloc_set_opts(nullptr,
                                    av_get_default_channel_layout(TARGET_CHANNELS), TARGET_SAMPLE_FORMAT, TARGET_SAMPLE_RATE,
                                    av_get_default_channel_layout(codecContext->channels), codecContext->sample_fmt, codecContext->sample_rate,
                                    0, nullptr);

    if (!swrContext || swr_init(swrContext) < 0) {
        LOGE("Failed to initialize the resampling context");
        return -1;
    }
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi initialize the resampling context");

    AVPacket packet;
    av_init_packet(&packet);
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi av_init_packet");

    while (true) {
        if ((ret = av_read_frame(formatContext, &packet)) < 0)
            break;
        __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi av_read_frame");

        if (packet.stream_index == audioStreamIndex) {
            ret = avcodec_send_packet(codecContext, &packet);

            if (ret < 0) {
                LOGE("Error sending a packet for decoding: %s", av_err2str(ret));
                break;
            }
            __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi sending packet for decoding");

            while (ret >= 0) {
                ret = avcodec_receive_frame(codecContext, frame);

                if (ret == AVERROR(EAGAIN)) {
                    // Le décodeur a besoin de plus de paquets pour générer un frame complet
                    break;
                } else if (ret == AVERROR_EOF) {
                    // Fin de l'encodage, tous les paquets ont été décodés
                    break;
                } else if (ret < 0) {
                    LOGE("Error during decoding: %s", av_err2str(ret));
                    return -1;
                }
                __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi decoding");

                convert_frame(swrContext, codecContext, frame, &data, &dataSize, false);
                __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi convert_frame");
            }
        }

        av_packet_unref(&packet);
        __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi packet_unref");
    }

    convert_frame(swrContext, codecContext, nullptr, &data, &dataSize, true);
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Réussi final convert frame");

    fd = open(outputPath, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        LOGE("Failed to open output file");
        return -1;
    }

    write_wave_hdr(fd, dataSize * sizeof(int16_t));
    write(fd, data, dataSize * sizeof(int16_t));
    AVFormatContext *pFormatCtx = avformat_alloc_context();
// Ouvrez le fichier audio et récupérez les informations sur le flux
    if(avformat_open_input(&pFormatCtx, outputPath, NULL, NULL) != 0){
        // Gérer l'erreur
    }
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0){
        // Gérer l'erreur
    }

// La durée du flux est stockée en microsecondes
    int64_t duration_us = pFormatCtx->duration;
// Convertir en secondes
    double duration_sec = ((double)duration_us) / AV_TIME_BASE;
    __android_log_print(ANDROID_LOG_VERBOSE, "Audio Conversion", "Durée en sec: %f", duration_sec);

    close(fd);
    avformat_close_input(&pFormatCtx);
    free(data);
    swr_free(&swrContext);
    av_frame_free(&frame);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);

    env->ReleaseStringUTFChars(inputFilePath, inputPath);
    env->ReleaseStringUTFChars(outputFilePath, outputPath);

    return 0;
}

// Example: load a tflite model using TF Lite C++ API
// Credit to https://github.com/ValYouW/crossplatform-tflite-object-detecion
// Credit to https://github.com/cuongvng/TF-Lite-Cpp-API-for-Android
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_audio2text_MainActivity_loadModelJNI(
        JNIEnv* env,
        jobject /* this */,
        jobject assetManager,
        jstring fileName) {

    //Load Whisper Model into buffer
    jstring result = NULL;
    struct timeval start_time,end_time;
    if(!g_whisper_tflite_params.is_whisper_tflite_initialized) {
        gettimeofday(&start_time, NULL);
        //const char *modelpathEncoder = "whisper-encoder-hybrid.tflite";
        //const char *modelpathDecoder = "whisper-decoder-language-hybrid.tflite";
        const char *modelpath = "whisper-small.tflite";
        if (!(env->IsSameObject(assetManager, NULL))) {
            AAssetManager *mgr = AAssetManager_fromJava(env, assetManager);
            AAsset *asset = AAssetManager_open(mgr, modelpath, AASSET_MODE_UNKNOWN);
            assert(asset != nullptr);

            g_whisper_tflite_params.size = AAsset_getLength(asset);
            g_whisper_tflite_params.buffer = (char *) malloc(sizeof(char) * g_whisper_tflite_params.size);
            AAsset_read(asset, g_whisper_tflite_params.buffer, g_whisper_tflite_params.size);
            AAsset_close(asset);

            /*AAsset *assetDecoder = AAssetManager_open(mgr, modelpathDecoder, AASSET_MODE_UNKNOWN);
            assert(assetDecoder != nullptr);

            g_whisper_tflite_decoder_params.size = AAsset_getLength(assetDecoder);
            g_whisper_tflite_decoder_params.buffer = (char *) malloc(sizeof(char) * g_whisper_tflite_decoder_params.size);
            AAsset_read(assetDecoder, g_whisper_tflite_decoder_params.buffer, g_whisper_tflite_decoder_params.size);
            AAsset_close(assetDecoder);*/
        }

        //Load filters and vocab data from preg enerated filters_vocab_gen.bin file
        const char *vocab_filename = "filters_vocab_multilingual.bin";


        if (!(env->IsSameObject(assetManager, NULL))) {
            AAssetManager *mgr = AAssetManager_fromJava(env, assetManager);
            AAsset *asset = AAssetManager_open(mgr, vocab_filename, AASSET_MODE_UNKNOWN);
            assert(asset != nullptr);
            uint32_t magic = 0;
            AAsset_read(asset, &magic, sizeof(magic));
            //@magic:USEN
            if (magic != 0x5553454e) {
                // printf("%s: invalid vocab file '%s' (bad magic)\n", __func__, fname.c_str());
                __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR",
                                    "%s: invalid vocab file '%s' (bad magic)\n", __func__,
                                    vocab_filename);
                return result;
            }
            // load mel filters
            {
                AAsset_read(asset, (char *) &filters.n_mel, sizeof(filters.n_mel));
                AAsset_read(asset, (char *) &filters.n_fft, sizeof(filters.n_fft));
                __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR", "%s: n_mel:%d n_fft:%d\n",
                                    __func__, filters.n_mel, filters.n_fft);
                filters.data.resize(filters.n_mel * filters.n_fft);
                AAsset_read(asset, (char *) filters.data.data(), filters.data.size() * sizeof(float));
            }

            int32_t n_vocab = 0;
            std::string word;
            // load vocab
            {
                AAsset_read(asset, (char *) &n_vocab, sizeof(n_vocab));
                g_vocab.n_vocab = n_vocab;
                __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR", "\nn_vocab:%d\n",
                                    (int) n_vocab);

                for (int i = 0; i < n_vocab; i++) {
                    uint32_t len;
                    AAsset_read(asset, (char *) &len, sizeof(len));

                    word.resize(len);
                    AAsset_read(asset, (char *) word.data(), len);
                    g_vocab.id_to_token[i] = word;
                    //printf("len:%d",(int)len);
                    //printf("'%s'\n", g_vocab.id_to_token[i].c_str());
                }

                g_vocab.n_vocab = 51864;//add additional vocab ids
                if (g_vocab.is_multilingual()) {
                    g_vocab.token_eot++;
                    g_vocab.token_sot++;
                    g_vocab.token_prev++;
                    g_vocab.token_solm++;
                    g_vocab.token_not++;
                    g_vocab.token_beg++;
                }
                for (int i = n_vocab; i < g_vocab.n_vocab; i++) {
                    if (i > g_vocab.token_beg) {
                        word = "[_TT_" + std::to_string(i - g_vocab.token_beg) + "]";
                    } else if (i == g_vocab.token_eot) {
                        word = "[_EOT_]";
                    } else if (i == g_vocab.token_sot) {
                        word = "[_SOT_]";
                    } else if (i == g_vocab.token_prev) {
                        word = "[_PREV_]";
                    } else if (i == g_vocab.token_not) {
                        word = "[_NOT_]";
                    } else if (i == g_vocab.token_beg) {
                        word = "[_BEG_]";
                    } else {
                        word = "[_extra_token_" + std::to_string(i) + "]";
                    }
                    g_vocab.id_to_token[i] = word;
                    // printf("%s: g_vocab[%d] = '%s'\n", __func__, i, word.c_str());
                }
            }

            AAsset_close(asset);
        }


        gettimeofday(&end_time, NULL);
        __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR",
                            "JNI mel filter extraction time %ld seconds \n",
                            (end_time.tv_sec - start_time.tv_sec));
}
    gettimeofday(&start_time, NULL);
    // WAV input
    std::vector<float> pcmf32;
    std::vector<std::vector<float>> segments;  // vecteur pour stocker les segments
    size_t segment_size = WHISPER_SAMPLE_RATE*WHISPER_CHUNK_SIZE;
    //Generate input_features for Audio file
    if (INFERENCE_ON_AUDIO_FILE) {
        const char* pcmfilename = env->GetStringUTFChars(fileName, 0);
        __android_log_print(ANDROID_LOG_VERBOSE, "Filename", "Le fichier est : %s", pcmfilename);
        {
            drwav wav;
            //drmp3 mp3;

            // Open the file in binary mode
            std::ifstream file(pcmfilename, std::ios::binary);

            // Read the file into a vector
            std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            // Now buffer contains the file data. If you need a char *, you can get it like this:
            char *wav_buffer = &buffer[0];

            if (!drwav_init_memory(&wav, wav_buffer, buffer.size(),NULL)) {
             __android_log_print(ANDROID_LOG_VERBOSE, "Niranjan", "failed to open WAV file '%s' - check your input\n", pcmfilename);
             return result;
          }
            if (wav.channels != 1 && wav.channels != 2) {
                __android_log_print(ANDROID_LOG_VERBOSE, "Niranjan", "WAV file '%s' must be mono or stereo\n", pcmfilename);

                return result;
            }

            if (wav.sampleRate != WHISPER_SAMPLE_RATE) {
                __android_log_print(ANDROID_LOG_VERBOSE, "Niranjan", "WWAV file '%s' must be 16 kHz\n", pcmfilename);
                return result;
            }

            if (wav.bitsPerSample != 16) {
                __android_log_print(ANDROID_LOG_VERBOSE, "Niranjan", "WAV file '%s' must be 16-bit\n", pcmfilename);
                return result;
            }

            int n = wav.totalPCMFrameCount;

            __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Nombre de frames: %d", n);

            std::vector<int16_t> pcm16;
            pcm16.resize(n*wav.channels);
            drwav_read_pcm_frames_s16(&wav, n, pcm16.data());
            drwav_uninit(&wav);
            // convert to mono, float
            pcmf32.resize(n);
            if (wav.channels == 1) {
                for (int i = 0; i < n; i++) {
                    pcmf32[i] = float(pcm16[i])/32768.0f;
                }
            } else {
                for (int i = 0; i < n; i++) {
                    pcmf32[i] = float(pcm16[2*i] + pcm16[2*i + 1])/65536.0f;
                }
            }

            double duration_in_seconds = (double)n / wav.sampleRate;
            __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR:", "Audio duration: %f seconds", duration_in_seconds);
        }
    }//end of audio file processing

    for (size_t i = 0; i < pcmf32.size(); i += segment_size) {
        size_t end = i + segment_size;
        if (end > pcmf32.size()) {
            end = pcmf32.size();
        }

        std::vector<float> segment(pcmf32.begin() + i, pcmf32.begin() + end);
        segments.push_back(segment);
    }

    gettimeofday(&end_time, NULL);
    __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR", "JNI (Spectrogram)input feature extraction time %ld seconds \n",(end_time.tv_sec-start_time.tv_sec));

    if(!g_whisper_tflite_params.is_whisper_tflite_initialized) {
        // Load tflite model buffer
        g_whisper_tflite_params.model =
                tflite::FlatBufferModel::BuildFromBuffer(g_whisper_tflite_params.buffer, g_whisper_tflite_params.size);
        TFLITE_MINIMAL_CHECK(g_whisper_tflite_params.model != nullptr);

        // Build the interpreter with the InterpreterBuilder.
        tflite::InterpreterBuilder builder(*(g_whisper_tflite_params.model), g_whisper_tflite_params.resolver);

        builder(&(g_whisper_tflite_params.interpreter));
        TFLITE_MINIMAL_CHECK(g_whisper_tflite_params.interpreter != nullptr);

        // NEW: Prepare GPU delegate.
        //  auto* delegate = TfLiteGpuDelegateV2Create(nullptr);
        // if (interpreter->ModifyGraphWithDelegate(delegate) != kTfLiteOk) {
        //     __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR", "gpu delegate failed \n");
        // }

        // Allocate tensor buffers.
        TFLITE_MINIMAL_CHECK(g_whisper_tflite_params.interpreter->AllocateTensors() == kTfLiteOk);

        g_whisper_tflite_params.input = g_whisper_tflite_params.interpreter->typed_input_tensor<float>(0);
        g_whisper_tflite_params.is_whisper_tflite_initialized = true;
    }
    /*if(!g_whisper_tflite_decoder_params.is_whisper_tflite_initialized) {
        g_whisper_tflite_decoder_params.model =
                tflite::FlatBufferModel::BuildFromBuffer(g_whisper_tflite_decoder_params.buffer, g_whisper_tflite_decoder_params.size);
        TFLITE_MINIMAL_CHECK(g_whisper_tflite_decoder_params.model != nullptr);

        tflite::InterpreterBuilder builderDecoder(*(g_whisper_tflite_decoder_params.model), g_whisper_tflite_decoder_params.resolver);
        builderDecoder(&(g_whisper_tflite_decoder_params.interpreter));

        TFLITE_MINIMAL_CHECK(g_whisper_tflite_decoder_params.interpreter != nullptr);
        TFLITE_MINIMAL_CHECK(g_whisper_tflite_decoder_params.interpreter->AllocateTensors() == kTfLiteOk);
        g_whisper_tflite_decoder_params.input = g_whisper_tflite_decoder_params.interpreter->typed_input_tensor<float>(0);
        //memcpy(g_whisper_tflite_decoder_params.inputOriginal, g_whisper_tflite_params.input, sizeof(g_whisper_tflite_decoder_params.input))
        g_whisper_tflite_decoder_params.is_whisper_tflite_initialized = true;
    } else {
        //memcpy(g_whisper_tflite_decoder_params.input, g_whisper_tflite_decoder_params.inputOriginal, sizeof(g_whisper_tflite_decoder_params.inputOriginal))
    }*/

    gettimeofday(&start_time, NULL);
    std::string text = "";
    int total_segments = pcmf32.size(); // Nombre total de segments
    __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Le nombre de segments est : %d", total_segments);

    int input = g_whisper_tflite_params.interpreter->inputs()[0];
    TfLiteTensor* input_tensor = g_whisper_tflite_params.interpreter->tensor(input);
    TfLiteIntArray* dims = input_tensor->dims;
    int tensor_size = 1;
    for (int i = 0; i < dims->size; ++i) {
        __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Dimension %d: %d", i, dims->data[i]);
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto processor_count = std::thread::hardware_concurrency();
        auto& segment = segments[i];  // Obtenir le segment courant
        __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Taille de segment: %d", segment.size());

        __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Stade: %d", i);
        // Calculez la taille de la tranche actuelle. Si nous sommes à la fin des données, elle pourrait être plus petite que `chunk_size`.
        int current_chunk_size = std::min(segment_size, segment.size());
        if (current_chunk_size < segment_size) {
            __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "On y est");
            // Si la tranche est plus petite que `chunk_size`, ajoutez des zéros pour l'aligner à `chunk_size`.
            segment.insert(segment.end(), WHISPER_SAMPLE_RATE*WHISPER_CHUNK_SIZE - segment.size(), 0);
        }

        // Remplacer pcmf32.data() par segment.data() pour log_mel_spectrogram
        if (!log_mel_spectrogram(segment.data(), segment.size(), WHISPER_SAMPLE_RATE, WHISPER_N_FFT, WHISPER_HOP_LENGTH, WHISPER_N_MEL, processor_count,filters, mel)) {
            fprintf(stderr, "%s: failed to compute mel spectrogram\n", __func__);
            return result;
        }

        // Copier un segment de données dans le tensor d'entrée
        if (INFERENCE_ON_AUDIO_FILE) {
            __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Essai de copie dans le buffer: %d", mel.data.data());
            __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Taille de mel data: %d", mel.data.size());

            memcpy(g_whisper_tflite_params.input, mel.data.data(), WHISPER_N_MEL * WHISPER_MEL_LEN * sizeof(float));
            __android_log_print(ANDROID_LOG_INFO, "Whisper ASR", "Copie réussie");
        } else {
            // Remarque: cette partie du code pourrait nécessiter une modification similaire
            memcpy(g_whisper_tflite_params.input, _content_input_features_bin, WHISPER_N_MEL*WHISPER_MEL_LEN*sizeof(float));
        }

        // Exécuter l'inférence
        if (g_whisper_tflite_params.interpreter->Invoke() != kTfLiteOk) return result;

            // Traiter le résultat
            int output = g_whisper_tflite_params.interpreter->outputs()[0];
            TfLiteTensor *output_tensor = g_whisper_tflite_params.interpreter->tensor(output);
            TfLiteIntArray *output_dims = output_tensor->dims;
            auto output_size = output_dims->data[output_dims->size - 1];
            int *output_int = g_whisper_tflite_params.interpreter->typed_output_tensor<int>(0);

            for (int j = 0; j < output_size; j++) {
                if(output_int[j] == g_vocab.token_eot){
                    break;
                }
                if((output_int[j] !=50257) && (output_int[j] !=50362))
                    text += whisper_token_to_str(output_int[j]);
            }

            __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR: part transcript", "\n%s\n", text.c_str());
        }
        __android_log_print(ANDROID_LOG_VERBOSE, "Whisper ASR", "\n%s\n", text.c_str());
        printf("\n");
        //std::string status = "Load TF Lite model successfully!";
        //free(buffer);
        return env->NewStringUTF(text.c_str());
    }