/* 
 * Copyright (C) 2014-2015 Ross Bencina
 * For conditions of distribution and use, see copyright notice in COPYING
 */
#if HAVE_CONFIG_H
#  include "config.h"
#endif
#if HAVE_STDINT_H
#  include <stdint.h>
#endif
#if HAVE_INTTYPES_H
#  include <inttypes.h>
#elif defined(_MSC_VER)
#  define SCNd64 "I64d"
#endif

#include <stdio.h>
#include <stdlib.h>

#if 0
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SIGACTION
#include <signal.h>
#endif
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "compat.h"
#include "pcm_reader.h"
#include "aacenc.h"
#include "m4af.h"
#include "progress.h"
#include "version.h"
#include "metadata.h"

#endif

#include "fdk-aac/aacdecoder_lib.h"

#define PROGNAME "fdkaac-decoder"


void aacdec_ext_save_decoder_state(HANDLE_AACDECODER decoder, const char *stateFileName)
{
    if (!stateFileName) {
        fprintf(stderr, "WARNING: no save decoder state file specified\n");
        return;
    }

    FILE *fp = fopen(stateFileName, "wb");
    if (!fp) {
        fprintf(stderr, "ERROR: aacdec_ext_save_encoder_state couldn't open state file %s\n", stateFileName);
        return;
    }

    aacDecoder_ExtSaveState(decoder, fp);

    fclose(fp);
}

void aacdec_ext_load_decoder_state(HANDLE_AACDECODER decoder, const char *stateFileName)
{
    if (!stateFileName) {
        fprintf(stderr, "WARNING: no load decoder state file specified\n");
        return;
    }

    FILE *fp = fopen(stateFileName, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: aacdec_ext_load_encoder_state couldn't open state file %s\n", stateFileName);
        return;
    }

    aacDecoder_ExtLoadState(decoder, fp);

    fclose(fp);
}


static void printUsage()
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "%s aac-adts-input-file pcm-output-file\n", PROGNAME);
    fprintf(stderr, "%s aac-adts-input-file pcm-output-file output-state-file\n", PROGNAME);
    fprintf(stderr, "%s aac-adts-input-file pcm-output-file input-state-file output-state-file\n", PROGNAME);
    fprintf(stderr, "(Output is raw 16-bit interleaved PCM with same sample rate and channel count as the AAC input.)\n");
}

int main(int argc, char **argv)
{
    int result = 0;

    if (argc < 3) {
        fprintf(stderr, "Not enough arguments.\n");
        printUsage();
        exit(-1);
    }

    if (argc > 5) {
        fprintf(stderr, "Too many arguments.\n");
        printUsage();
        exit(-1);
    }

    const char *inputAdtsFileName = argv[1];
    const char *outputPcmFileName = argv[2];

    const char *inputStateFileName = 0;
    const char *outputStateFileName = 0;

    if (argc == 4) {
        outputStateFileName = argv[3];    
    } else if (argc == 5) {
        inputStateFileName = argv[3];
        outputStateFileName = argv[4];
    }

    ////
    // the following are cleaned up at end below
    FILE *inFile=0;
    FILE *outFile=0;
    HANDLE_AACDECODER decoder=0;
    //

    inFile = fopen(inputAdtsFileName, "rb");
    if (!inFile) {
        fprintf(stderr, "ERROR: could not open input file\n");
        result = -1;
        goto end;
    }

    outFile = fopen(outputPcmFileName, "wb");
    if (!outFile) {
        fprintf(stderr, "ERROR: could not open output file\n");
        result = -1;
        goto end;
    }

    int shouldSaveDecoderState = 0;

    decoder = aacDecoder_Open(TT_MP4_ADTS, /*nrOfLayers=*/1);
#if 0
    // DEBUG: save and load the decoder state as a test:
    aacdec_ext_save_decoder_state(decoder, "test.decoderstate");
    aacdec_ext_load_decoder_state(decoder, "test.decoderstate");
#endif

#define INPUT_BUFFER_SIZE (1024 * 32)
#define OUTPUT_BUFFER_SIZE (1024 * 32)

    UCHAR inputBuffer[INPUT_BUFFER_SIZE];
    UINT inputBufferSize = INPUT_BUFFER_SIZE;

    INT_PCM outputBuffer[OUTPUT_BUFFER_SIZE];

    if (inputStateFileName) {
#if 1
        // aacdec_ext_load_decoder_state() requires that the decoder has already allocated the
        // correct channel configuration. This could be done via aacDecoder_ConfigRaw()
        // or aacDecoder_Config. As a simple solution we let the decoder pull the data from
        // the bitstream, then rewind the file.

        // FIXME: ideally we would like aacdec_ext_load_decoder_state() to handle allocating
        // any needed state. work has begun on this but it is incomplete.
        // for example SBR_DECODER_ELEMENT is not currently allocated without decoding a frame.

        // FIXME: we don't need to read the first input buffer twice.
        size_t bytesRead = fread(inputBuffer, 1, INPUT_BUFFER_SIZE, inFile);
        if (bytesRead == 0) {
            fprintf(stderr, "ERROR: empty file\n");
            result = -1;
            goto end;
        }

        {
            UINT bytesValid = bytesRead;

            UCHAR *in = inputBuffer;
            UINT inSize = bytesRead;

            // decode one frame...

            UINT oldBytesValid = bytesValid;
            AAC_DECODER_ERROR err = aacDecoder_Fill(decoder, &in, &inSize, &bytesValid);
            if (err != AAC_DEC_OK) {
                fprintf(stderr, "ERROR: decoder (fill) returned error 0x%04x\n", err);
                result = -1;
                goto end;
            }

            UINT delta = oldBytesValid - bytesValid;
            //printf("aacDecoder_Fill consumed %d bytes\n", delta);
            in += delta;
            inSize -= delta;
            if (delta > 0)
                shouldSaveDecoderState = 1;

            err = aacDecoder_DecodeFrame(decoder, outputBuffer, OUTPUT_BUFFER_SIZE, /*flags=*/0);
            if (err == AAC_DEC_NOT_ENOUGH_BITS) {
                fprintf(stderr, "ERROR: file only contains incomplete frame (unsupported)\n", err);
                goto end;
            }
            else if (err != AAC_DEC_OK) {
                fprintf(stderr, "ERROR: decoder (decode frame) returned error 0x%04x\n", err);
                result = -1;
                goto end;
            }

            aacDecoder_DecodeFrame(decoder, outputBuffer, OUTPUT_BUFFER_SIZE, /*flags=*/AACDEC_FLUSH);
        }

        fseek(inFile, 0, SEEK_SET); // rewind to start
#endif

        aacdec_ext_load_decoder_state(decoder, inputStateFileName);
        shouldSaveDecoderState = 1;
    }

    do {
        size_t bytesRead = fread(inputBuffer, 1, INPUT_BUFFER_SIZE, inFile);
        if (bytesRead == 0) {
            goto end;
        }

        UINT bytesValid = bytesRead;

        UCHAR *in = inputBuffer;
        UINT inSize = bytesRead;

        do {
            UINT oldBytesValid = bytesValid;
            AAC_DECODER_ERROR err = aacDecoder_Fill(decoder, &in, &inSize, &bytesValid);
            if (err != AAC_DEC_OK) {
                fprintf(stderr, "ERROR: decoder (fill) returned error 0x%04x\n", err);
                result = -1;
                goto end;
            }

            UINT delta = oldBytesValid - bytesValid;
            //printf("aacDecoder_Fill consumed %d bytes\n", delta);
            in += delta;
            inSize -= delta;
            if (delta > 0)
                shouldSaveDecoderState = 1;

            err = aacDecoder_DecodeFrame(decoder, outputBuffer, OUTPUT_BUFFER_SIZE, /*flags=*/0);
            if (err == AAC_DEC_NOT_ENOUGH_BITS) {
                goto end;
            } else if (err != AAC_DEC_OK) {
                fprintf(stderr, "ERROR: decoder (decode frame) returned error 0x%04x\n", err);
                result = -1;
                goto end;
            }
            
            CStreamInfo* streamInfo = aacDecoder_GetStreamInfo(decoder);

            size_t outBytes = streamInfo->numChannels * streamInfo->frameSize * sizeof(INT_PCM);
            size_t bytesWritten = fwrite(outputBuffer, 1, outBytes, outFile);
            if (bytesWritten != outBytes) {
                fprintf(stderr, "ERROR: error writing output\n");
                result = -1;
                goto end;
            }

#if 0
            // DEBUG: save/restore state after each frame to flush out persistence errors
            aacdec_ext_save_decoder_state(decoder, outputStateFileName);
            err = aacDecoder_DecodeFrame(decoder, outputBuffer, OUTPUT_BUFFER_SIZE, /*flags=*/AACDEC_CLRHIST); //AACDEC_FLUSH
            aacdec_ext_load_decoder_state(decoder, outputStateFileName); //  verify that state loads
            /*
            // save out again to check that load was correct
            char s[1000];
            strcpy(s, outputStateFileName);
            strcat(s, "x");
            aacdec_ext_save_decoder_state(decoder, s);
            */
#endif
        } while (bytesValid > 0);

    } while (1);

end:
    if (decoder) {
        if (shouldSaveDecoderState && outputStateFileName){
            aacdec_ext_save_decoder_state(decoder, outputStateFileName);
            //aacdec_ext_load_decoder_state(decoder, outputStateFileName); // DEBUG: verify that state loads
        }

        aacDecoder_Close(decoder);
    }

    if (inFile) fclose(inFile);
    if (outFile) fclose(outFile);

    return result;
}
