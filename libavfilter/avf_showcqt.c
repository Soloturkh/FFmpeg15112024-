/*
 * Copyright (c) 2014 Muhammad Faiz <mfcc64@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavcodec/avfft.h"
#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/xga_font_data.h"
#include "libavutil/qsort.h"
#include "libavutil/time.h"
#include "avfilter.h"
#include "internal.h"

#include <math.h>
#include <stdlib.h>

/* this filter is designed to do 16 bins/semitones constant Q transform with Brown-Puckette algorithm
 * start from E0 to D#10 (10 octaves)
 * so there are 16 bins/semitones * 12 semitones/octaves * 10 octaves = 1920 bins
 * match with full HD resolution */

#define VIDEO_WIDTH 1920
#define VIDEO_HEIGHT 1080
#define FONT_HEIGHT 32
#define SPECTOGRAM_HEIGHT ((VIDEO_HEIGHT-FONT_HEIGHT)/2)
#define SPECTOGRAM_START (VIDEO_HEIGHT-SPECTOGRAM_HEIGHT)
#define BASE_FREQ 20.051392800492
#define COEFF_CLAMP 1.0e-4

typedef struct {
    FFTSample value;
    int index;
} SparseCoeff;

static inline int qsort_sparsecoeff(const SparseCoeff *a, const SparseCoeff *b)
{
    if (fabsf(a->value) >= fabsf(b->value))
        return 1;
    else
        return -1;
}

typedef struct {
    const AVClass *class;
    AVFrame *outpicref;
    FFTContext *fft_context;
    FFTComplex *fft_data;
    FFTComplex *fft_result_left;
    FFTComplex *fft_result_right;
    SparseCoeff *coeff_sort;
    SparseCoeff *coeffs[VIDEO_WIDTH];
    int coeffs_len[VIDEO_WIDTH];
    uint8_t font_color[VIDEO_WIDTH];
    uint8_t spectogram[SPECTOGRAM_HEIGHT][VIDEO_WIDTH][3];
    int64_t frame_count;
    int spectogram_count;
    int spectogram_index;
    int fft_bits;
    int req_fullfilled;
    int remaining_fill;
    double volume;
    double timeclamp;   /* lower timeclamp, time-accurate, higher timeclamp, freq-accurate (at low freq)*/
    float coeffclamp;   /* lower coeffclamp, more precise, higher coeffclamp, faster */
    float gamma;        /* lower gamma, more contrast, higher gamma, more range */
    int fps;            /* the required fps is so strict, so it's enough to be int, but 24000/1001 etc cannot be encoded */
    int count;          /* fps * count = transform rate */
} ShowCQTContext;

#define OFFSET(x) offsetof(ShowCQTContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption showcqt_options[] = {
    { "volume", "set volume", OFFSET(volume), AV_OPT_TYPE_DOUBLE, { .dbl = 16 }, 0.1, 100, FLAGS },
    { "timeclamp", "set timeclamp", OFFSET(timeclamp), AV_OPT_TYPE_DOUBLE, { .dbl = 0.17 }, 0.1, 1.0, FLAGS },
    { "coeffclamp", "set coeffclamp", OFFSET(coeffclamp), AV_OPT_TYPE_FLOAT, { .dbl = 1 }, 0.1, 10, FLAGS },
    { "gamma", "set gamma", OFFSET(gamma), AV_OPT_TYPE_FLOAT, { .dbl = 3 }, 1, 7, FLAGS },
    { "fps", "set video fps", OFFSET(fps), AV_OPT_TYPE_INT, { .i64 = 25 }, 10, 100, FLAGS },
    { "count", "set number of transform per frame", OFFSET(count), AV_OPT_TYPE_INT, { .i64 = 6 }, 1, 30, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(showcqt);

static av_cold void uninit(AVFilterContext *ctx)
{
    int k;
    ShowCQTContext *s = ctx->priv;
    av_fft_end(s->fft_context);
    s->fft_context = NULL;
    for (k = 0; k < VIDEO_WIDTH; k++)
        av_freep(&s->coeffs[k]);
    av_freep(&s->fft_data);
    av_freep(&s->fft_result_left);
    av_freep(&s->fft_result_right);
    av_freep(&s->coeff_sort);
    av_frame_free(&s->outpicref);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE };
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_RGB24, AV_PIX_FMT_NONE };
    static const int64_t channel_layouts[] = { AV_CH_LAYOUT_STEREO, AV_CH_LAYOUT_STEREO_DOWNMIX, -1 };
    static const int samplerates[] = { 44100, 48000, -1 };

    /* set input audio formats */
    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ff_formats_ref(formats, &inlink->out_formats);

    layouts = avfilter_make_format64_list(channel_layouts);
    if (!layouts)
        return AVERROR(ENOMEM);
    ff_channel_layouts_ref(layouts, &inlink->out_channel_layouts);

    formats = ff_make_format_list(samplerates);
    if (!formats)
        return AVERROR(ENOMEM);
    ff_formats_ref(formats, &inlink->out_samplerates);

    /* set output video format */
    formats = ff_make_format_list(pix_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ff_formats_ref(formats, &outlink->in_formats);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    ShowCQTContext *s = ctx->priv;
    int fft_len, k, x, y;
    int num_coeffs = 0;
    int rate = inlink->sample_rate;
    double max_len = rate * (double) s->timeclamp;
    int64_t start_time, end_time;
    s->fft_bits = ceil(log2(max_len));
    fft_len = 1 << s->fft_bits;

    if (rate % (s->fps * s->count))
    {
        av_log(ctx, AV_LOG_ERROR, "Rate (%u) is not divisible by fps*count (%u*%u)\n", rate, s->fps, s->count);
        return AVERROR(EINVAL);
    }

    s->fft_data = av_malloc_array(fft_len, sizeof(*s->fft_data));
    s->coeff_sort = av_malloc_array(fft_len, sizeof(*s->coeff_sort));
    s->fft_result_left = av_malloc_array(fft_len, sizeof(*s->fft_result_left));
    s->fft_result_right = av_malloc_array(fft_len, sizeof(*s->fft_result_right));
    s->fft_context = av_fft_init(s->fft_bits, 0);

    if (!s->fft_data || !s->coeff_sort || !s->fft_result_left || !s->fft_result_right || !s->fft_context)
        return AVERROR(ENOMEM);

    /* initializing font */
    for (x = 0; x < VIDEO_WIDTH; x++)
    {
        if (x >= (12*3+8)*16 && x < (12*4+8)*16)
        {
            float fx = (x-(12*3+8)*16) * (1.0f/192.0f);
            float sv = sinf(M_PI*fx);
            s->font_color[x] = sv*sv*255.0f + 0.5f;
        }
        else
            s->font_color[x] = 0;
    }

    av_log(ctx, AV_LOG_INFO, "Calculating spectral kernel, please wait\n");
    start_time = av_gettime_relative();
    for (k = 0; k < VIDEO_WIDTH; k++)
    {
        int hlen = fft_len >> 1;
        float total = 0;
        float partial = 0;
        double freq = BASE_FREQ * exp2(k * (1.0/192.0));
        double tlen = rate * (24.0 * 16.0) /freq;
        /* a window function from Albert H. Nuttall,
         * "Some Windows with Very Good Sidelobe Behavior"
         * -93.32 dB peak sidelobe and 18 dB/octave asymptotic decay
         * coefficient normalized to a0 = 1 */
        double a0 = 0.355768;
        double a1 = 0.487396/a0;
        double a2 = 0.144232/a0;
        double a3 = 0.012604/a0;
        double sv_step, cv_step, sv, cv;
        double sw_step, cw_step, sw, cw, w;

        tlen = tlen * max_len / (tlen + max_len);
        s->fft_data[0].re = 0;
        s->fft_data[0].im = 0;
        s->fft_data[hlen].re = (1.0 + a1 + a2 + a3) * (1.0/tlen) * s->volume * (1.0/fft_len);
        s->fft_data[hlen].im = 0;
        sv_step = sv = sin(2.0*M_PI*freq*(1.0/rate));
        cv_step = cv = cos(2.0*M_PI*freq*(1.0/rate));
        /* also optimizing window func */
        sw_step = sw = sin(2.0*M_PI*(1.0/tlen));
        cw_step = cw = cos(2.0*M_PI*(1.0/tlen));
        for (x = 1; x < 0.5 * tlen; x++)
        {
            double cv_tmp, cw_tmp;
            double cw2, cw3, sw2;

            cw2 = cw * cw - sw * sw;
            sw2 = cw * sw + sw * cw;
            cw3 = cw * cw2 - sw * sw2;
            w = (1.0 + a1 * cw + a2 * cw2 + a3 * cw3) * (1.0/tlen) * s->volume * (1.0/fft_len);
            s->fft_data[hlen + x].re = w * cv;
            s->fft_data[hlen + x].im = w * sv;
            s->fft_data[hlen - x].re = s->fft_data[hlen + x].re;
            s->fft_data[hlen - x].im = -s->fft_data[hlen + x].im;

            cv_tmp = cv * cv_step - sv * sv_step;
            sv = sv * cv_step + cv * sv_step;
            cv = cv_tmp;
            cw_tmp = cw * cw_step - sw * sw_step;
            sw = sw * cw_step + cw * sw_step;
            cw = cw_tmp;
        }
        for (; x < hlen; x++)
        {
            s->fft_data[hlen + x].re = 0;
            s->fft_data[hlen + x].im = 0;
            s->fft_data[hlen - x].re = 0;
            s->fft_data[hlen - x].im = 0;
        }
        av_fft_permute(s->fft_context, s->fft_data);
        av_fft_calc(s->fft_context, s->fft_data);

        for (x = 0; x < fft_len; x++)
        {
            s->coeff_sort[x].index = x;
            s->coeff_sort[x].value = s->fft_data[x].re;
        }

        AV_QSORT(s->coeff_sort, fft_len, SparseCoeff, qsort_sparsecoeff);
        for (x = 0; x < fft_len; x++)
            total += fabsf(s->coeff_sort[x].value);

        for (x = 0; x < fft_len; x++)
        {
            partial += fabsf(s->coeff_sort[x].value);
            if (partial > (total * s->coeffclamp * COEFF_CLAMP))
            {
                s->coeffs_len[k] = fft_len - x;
                num_coeffs += s->coeffs_len[k];
                s->coeffs[k] = av_malloc_array(s->coeffs_len[k], sizeof(*s->coeffs[k]));
                if (!s->coeffs[k])
                    return AVERROR(ENOMEM);
                for (y = 0; y < s->coeffs_len[k]; y++)
                    s->coeffs[k][y] = s->coeff_sort[x+y];
                break;
            }
        }
    }
    end_time = av_gettime_relative();
    av_log(ctx, AV_LOG_INFO, "Elapsed time %.6f s (fft_len=%u, num_coeffs=%u)\n", 1e-6 * (end_time-start_time), fft_len, num_coeffs);

    outlink->w = VIDEO_WIDTH;
    outlink->h = VIDEO_HEIGHT;

    s->req_fullfilled = 0;
    s->spectogram_index = 0;
    s->frame_count = 0;
    s->spectogram_count = 0;
    s->remaining_fill = fft_len >> 1;
    memset(s->spectogram, 0, VIDEO_WIDTH * SPECTOGRAM_HEIGHT * 3);
    memset(s->fft_data, 0, fft_len * sizeof(*s->fft_data));

    s->outpicref = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!s->outpicref)
        return AVERROR(ENOMEM);

    outlink->sample_aspect_ratio = av_make_q(1, 1);
    outlink->time_base = av_make_q(1, s->fps);
    outlink->frame_rate = av_make_q(s->fps, 1);
    return 0;
}

static int plot_cqt(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ShowCQTContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int fft_len = 1 << s->fft_bits;
    FFTSample result[VIDEO_WIDTH][4];
    int x, y, ret = 0;

    /* real part contains left samples, imaginary part contains right samples */
    memcpy(s->fft_result_left, s->fft_data, fft_len * sizeof(*s->fft_data));
    av_fft_permute(s->fft_context, s->fft_result_left);
    av_fft_calc(s->fft_context, s->fft_result_left);

    /* separate left and right, (and multiply by 2.0) */
    s->fft_result_right[0].re = 2.0f * s->fft_result_left[0].im;
    s->fft_result_right[0].im = 0;
    s->fft_result_left[0].re = 2.0f * s->fft_result_left[0].re;
    s->fft_result_left[0].im = 0;
    for (x = 1; x <= (fft_len >> 1); x++)
    {
        FFTSample tmpy = s->fft_result_left[fft_len-x].im - s->fft_result_left[x].im;

        s->fft_result_right[x].re = s->fft_result_left[x].im + s->fft_result_left[fft_len-x].im;
        s->fft_result_right[x].im = s->fft_result_left[x].re - s->fft_result_left[fft_len-x].re;
        s->fft_result_right[fft_len-x].re = s->fft_result_right[x].re;
        s->fft_result_right[fft_len-x].im = -s->fft_result_right[x].im;

        s->fft_result_left[x].re = s->fft_result_left[x].re + s->fft_result_left[fft_len-x].re;
        s->fft_result_left[x].im = tmpy;
        s->fft_result_left[fft_len-x].re = s->fft_result_left[x].re;
        s->fft_result_left[fft_len-x].im = -s->fft_result_left[x].im;
    }

    /* calculating cqt */
    for (x = 0; x < VIDEO_WIDTH; x++)
    {
        int u;
        float g = 1.0f / s->gamma;
        FFTComplex l = {0,0};
        FFTComplex r = {0,0};

        for (u = 0; u < s->coeffs_len[x]; u++)
        {
            FFTSample value = s->coeffs[x][u].value;
            int index = s->coeffs[x][u].index;
            l.re += value * s->fft_result_left[index].re;
            l.im += value * s->fft_result_left[index].im;
            r.re += value * s->fft_result_right[index].re;
            r.im += value * s->fft_result_right[index].im;
        }
        /* result is power, not amplitude */
        result[x][0] = l.re * l.re + l.im * l.im;
        result[x][2] = r.re * r.re + r.im * r.im;
        result[x][1] = 0.5f * (result[x][0] + result[x][2]);
        result[x][3] = result[x][1];
        result[x][0] = 255.0f * powf(FFMIN(1.0f,result[x][0]), g);
        result[x][1] = 255.0f * powf(FFMIN(1.0f,result[x][1]), g);
        result[x][2] = 255.0f * powf(FFMIN(1.0f,result[x][2]), g);
    }

    for (x = 0; x < VIDEO_WIDTH; x++)
    {
        s->spectogram[s->spectogram_index][x][0] = result[x][0] + 0.5f;
        s->spectogram[s->spectogram_index][x][1] = result[x][1] + 0.5f;
        s->spectogram[s->spectogram_index][x][2] = result[x][2] + 0.5f;
    }

    /* drawing */
    if (!s->spectogram_count)
    {
        uint8_t *data = (uint8_t*) s->outpicref->data[0];
        int linesize = s->outpicref->linesize[0];
        float rcp_result[VIDEO_WIDTH];

        for (x = 0; x < VIDEO_WIDTH; x++)
            rcp_result[x] = 1.0f / (result[x][3]+0.0001f);

        /* drawing bar */
        for (y = 0; y < SPECTOGRAM_HEIGHT; y++)
        {
            float height = (SPECTOGRAM_HEIGHT - y) * (1.0f/SPECTOGRAM_HEIGHT);
            uint8_t *lineptr = data + y * linesize;
            for (x = 0; x < VIDEO_WIDTH; x++)
            {
                float mul;
                if (result[x][3] <= height)
                {
                    *lineptr++ = 0;
                    *lineptr++ = 0;
                    *lineptr++ = 0;
                }
                else
                {
                    mul = (result[x][3] - height) * rcp_result[x];
                    *lineptr++ = mul * result[x][0] + 0.5f;
                    *lineptr++ = mul * result[x][1] + 0.5f;
                    *lineptr++ = mul * result[x][2] + 0.5f;
                }
            }

        }

        /* drawing font */
        for (y = 0; y < FONT_HEIGHT; y++)
        {
            uint8_t *lineptr = data + (SPECTOGRAM_HEIGHT + y) * linesize;
            memcpy(lineptr, s->spectogram[s->spectogram_index], VIDEO_WIDTH*3);
        }
        for (x = 0; x < VIDEO_WIDTH; x += VIDEO_WIDTH/10)
        {
            int u;
            static const char str[] = "EF G A BC D ";
            uint8_t *startptr = data + SPECTOGRAM_HEIGHT * linesize + x * 3;
            for (u = 0; str[u]; u++)
            {
                int v;
                for (v = 0; v < 16; v++)
                {
                    uint8_t *p = startptr + 2 * v * linesize + 16 * 3 * u;
                    int ux = x + 16 * u;
                    int mask;
                    for (mask = 0x80; mask; mask >>= 1)
                    {
                        if (mask & avpriv_vga16_font[str[u] * 16 + v])
                        {
                            p[0] = p[linesize] = 255 - s->font_color[ux];
                            p[1] = p[linesize+1] = 0;
                            p[2] = p[linesize+2] = s->font_color[ux];
                            p[3] = p[linesize+3] = 255 - s->font_color[ux+1];
                            p[4] = p[linesize+4] = 0;
                            p[5] = p[linesize+5] = s->font_color[ux+1];
                        }
                        p += 6;
                        ux += 2;
                    }
                }
            }

        }

        /* drawing spectogram/sonogram */
        if (linesize == VIDEO_WIDTH * 3)
        {
            int total_length = VIDEO_WIDTH * SPECTOGRAM_HEIGHT * 3;
            int back_length = VIDEO_WIDTH * s->spectogram_index * 3;
            data += SPECTOGRAM_START * VIDEO_WIDTH * 3;
            memcpy(data, s->spectogram[s->spectogram_index], total_length - back_length);
            data += total_length - back_length;
            if(back_length)
                memcpy(data, s->spectogram[0], back_length);
        }
        else
        {
            for (y = 0; y < SPECTOGRAM_HEIGHT; y++)
                memcpy(data + (SPECTOGRAM_START + y) * linesize, s->spectogram[(s->spectogram_index + y) % SPECTOGRAM_HEIGHT], VIDEO_WIDTH * 3);
        }

        s->outpicref->pts = s->frame_count;
        ret = ff_filter_frame(outlink, av_frame_clone(s->outpicref));
        s->req_fullfilled = 1;
        s->frame_count++;
    }
    s->spectogram_count = (s->spectogram_count + 1) % s->count;
    s->spectogram_index = (s->spectogram_index + SPECTOGRAM_HEIGHT - 1) % SPECTOGRAM_HEIGHT;
    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *insamples)
{
    AVFilterContext *ctx = inlink->dst;
    ShowCQTContext *s = ctx->priv;
    int step = inlink->sample_rate / (s->fps * s->count);
    int fft_len = 1 << s->fft_bits;
    int remaining;
    float *audio_data;

    if (!insamples)
    {
        while (s->remaining_fill < (fft_len >> 1))
        {
            int ret, x;
            memset(&s->fft_data[fft_len - s->remaining_fill], 0, sizeof(*s->fft_data) * s->remaining_fill);
            ret = plot_cqt(inlink);
            if (ret < 0)
                return ret;
            for (x = 0; x < (fft_len-step); x++)
                s->fft_data[x] = s->fft_data[x+step];
            s->remaining_fill += step;
        }
        return AVERROR(EOF);
    }

    remaining = insamples->nb_samples;
    audio_data = (float*) insamples->data[0];

    while (remaining)
    {
        if (remaining >= s->remaining_fill)
        {
            int i = insamples->nb_samples - remaining;
            int j = fft_len - s->remaining_fill;
            int m, ret;
            for (m = 0; m < s->remaining_fill; m++)
            {
                s->fft_data[j+m].re = audio_data[2*(i+m)];
                s->fft_data[j+m].im = audio_data[2*(i+m)+1];
            }
            ret = plot_cqt(inlink);
            if (ret < 0)
            {
                av_frame_free(&insamples);
                return ret;
            }
            remaining -= s->remaining_fill;
            for (m = 0; m < fft_len-step; m++)
                s->fft_data[m] = s->fft_data[m+step];
            s->remaining_fill = step;
        }
        else
        {
            int i = insamples->nb_samples - remaining;
            int j = fft_len - s->remaining_fill;
            int m;
            for (m = 0; m < remaining; m++)
            {
                s->fft_data[m+j].re = audio_data[2*(i+m)];
                s->fft_data[m+j].im = audio_data[2*(i+m)+1];
            }
            s->remaining_fill -= remaining;
            remaining = 0;
        }
    }
    av_frame_free(&insamples);
    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    ShowCQTContext *s = outlink->src->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    int ret;

    s->req_fullfilled = 0;
    do {
        ret = ff_request_frame(inlink);
    } while (!s->req_fullfilled && ret >= 0);

    if (ret == AVERROR_EOF && s->outpicref)
        filter_frame(inlink, NULL);
    return ret;
}

static const AVFilterPad showcqt_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad showcqt_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_avf_showcqt = {
    .name          = "showcqt",
    .description   = NULL_IF_CONFIG_SMALL("Convert input audio to a CQT (Constant Q Transform) spectrum video output."),
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(ShowCQTContext),
    .inputs        = showcqt_inputs,
    .outputs       = showcqt_outputs,
    .priv_class    = &showcqt_class,
};
