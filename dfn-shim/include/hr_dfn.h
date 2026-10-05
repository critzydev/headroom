/* C interface of dfn-shim: a thin, panic-safe layer over DeepFilterNet's libDF. SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef HR_DFN_H
#define HR_DFN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A loaded model: the parsed network of the first state, and the path to parse it again for the next ones. */
typedef struct HrDfModel HrDfModel;

/* One running stream: mono, 48 kHz for the shipped models, owns all recurrent state. Use from one thread at a time. */
typedef struct HrDfState HrDfState;

/* Loads a DeepFilterNet .tar.gz model (the ONNX export). Returns NULL on any failure; if err is not NULL a short
   message is written to it (at most errLen bytes, always terminated). Slow (the network is parsed and optimised):
   call it from a background thread. */
HrDfModel* hr_df_model_load (const char* utf8Path, char* err, size_t errLen);
void hr_df_model_free (HrDfModel*);

size_t hr_df_model_sample_rate (const HrDfModel*);   /* 48000 */
size_t hr_df_model_frame_length (const HrDfModel*);  /* hop size: 480 samples = 10 ms */
size_t hr_df_model_latency (const HrDfModel*);       /* samples between an input sample and its output sample, counted
                                                        from the moment the frame that holds it has been handed in:
                                                        (fft - hop) + lookahead * hop. 480 for LL, 1440 for standard. */

/* New independent stream in its initial state, NULL on failure. The first state of a model takes the network that
   hr_df_model_load parsed (instant); each further one parses the file again (0.15-0.5 s: call it from a background
   thread), because tract's tensors are reference counted without atomics: two states that shared any of them could
   not run on different threads. Calls for one model must be serialised by the caller. */
HrDfState* hr_df_state_new (const HrDfModel*);
void hr_df_state_free (HrDfState*);

/* Processes one frame of hr_df_model_frame_length samples (float, +-1.0). in and out must not overlap. Returns the
   frame's local SNR estimate in dB, or a NaN if the state failed (it then keeps passing input through until freed). */
float hr_df_state_process (HrDfState*, const float* in, float* out);

/* Post filter strength: 0 = off (default), useful range up to 0.05. */
void hr_df_state_set_post_filter (HrDfState*, float beta);

/* Attenuation limit in dB; >= 100 = none (default). Below 0.01 libDF copies input to output with no delay: avoid. */
void hr_df_state_set_atten_limit (HrDfState*, float db);

/* libDF's local-SNR thresholds, in dB of the network's own estimate for a frame (-15 .. 35). Below minDb the frame is
   muted (a zero mask); above maxErbDb it is passed untouched; above maxDfDb only the first stage (the ERB gains) runs
   and the second stage (deep filtering, which decides everything below ~4.8 kHz) is skipped. libDF's defaults are
   -10, 30 and 20 (what a new state has). */
void hr_df_state_set_thresholds (HrDfState*, float minDb, float maxErbDb, float maxDfDb);

#ifdef __cplusplus
}
#endif

#endif
