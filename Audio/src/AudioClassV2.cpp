// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.

#include "AudioClassV2.h"
#include <stdint.h>
#include <stdlib.h>

#include "nau88c10.h"
#include "stm32f4xx_hal.h"
#include "stm32412g_discovery.h"
#include "stm32412g_discovery_audio.h"
#include "Arduino.h"

static uint32_t _sampleRate;
static uint16_t _sampleBitDepth;
static uint8_t _channels;
static uint8_t _durationInSeconds;

static uint16_t _rx_buffer[BATCH_TRANSMIT_SIZE];
static uint16_t _tx_buffer[BATCH_TRANSMIT_SIZE];
static uint8_t  _record_buffer[BATCH_TRANSMIT_SIZE * 2];

const int Play_Chunk = 512;
static char _play_buffer[Play_Chunk];

static char * _wavFile;
static char * _recordCursor;
static uint16_t * _playCursor;
static int _audioFileSize;
static int _audioRemSize;

static volatile char _flag = 0;
static AUDIO_STATE_TypeDef _audioState;

static callbackFunc audioCallbackFptr = NULL;
static callbackFunc recordCallbackFptr = NULL;

AudioClass::AudioClass()
{
    format(DEFAULT_SAMPLE_RATE, DEFAULT_BITS_PER_SAMPLE);
    //rxBuffer = & _rx_buffer[0];
}

/* 
 * @brief Configure the audio data format
*/
void AudioClass::format(unsigned int sampleRate, unsigned short sampleBitLength)
{
    _sampleRate = sampleRate;
    _sampleBitDepth = sampleBitLength;
    _channels = STEREO;

    // Currently we ONLY support 16 bit depth audio sample
    unsigned short bitDepth;
    if (sampleBitLength == 16)
    {
        bitDepth = I2S_DATAFORMAT_16B;
    }
    else if (sampleBitLength == 24)
    {
        // TODO
        bitDepth = I2S_DATAFORMAT_24B;
    }
    else if (sampleBitLength == 32)
    {
        // TODO
        bitDepth = I2S_DATAFORMAT_32B;
    }
    else
    {
        bitDepth = I2S_DATAFORMAT_16B;
    }

    BSP_AUDIO_IN_OUT_Init(OUTPUT_DEVICE_AUTO, bitDepth, sampleRate);

    // Set audio initialization state
    _audioState = AUDIO_STATE_INIT;
}

/*
** @brief Start recording audio data using underlying codec
** @param Pointer to audio file
** @param fileSize Size of audio file
** @param durationInSeconds Audio duration to be recorded
** @retval AUDIO_OK if correct recording, else wrong recording
*/
int AudioClass::startRecord(char *audioFile, int fileSize, int durationInSeconds)
{
    if ((audioFile == NULL) || (fileSize < WAVE_HEADER_SIZE) || (durationInSeconds <= 0)) {
        return AUDIO_ERROR;
    }

    _flag = 0;
    _wavFile = audioFile;
    _audioFileSize = fileSize;
    _durationInSeconds = durationInSeconds;
    _recordCursor = _wavFile + WAVE_HEADER_SIZE;

    // Calculate the pcm data size to be recorded per given duration
    int pcmAudioSize = _sampleRate * _channels * _sampleBitDepth * _durationInSeconds / 8;
    if (pcmAudioSize < _audioFileSize - WAVE_HEADER_SIZE)
    {
        _audioFileSize = pcmAudioSize + WAVE_HEADER_SIZE;
    }

    _audioState = AUDIO_STATE_RECORDING;
    memset(_tx_buffer, 0x0, BATCH_TRANSMIT_SIZE*2);

    // Start DMA transfer
    BSP_AUDIO_In_Out_Transfer(_tx_buffer, _rx_buffer, BATCH_TRANSMIT_SIZE);

    return AUDIO_OK;
}

int AudioClass::startRecord()
{
    _audioState = AUDIO_STATE_RECORDING;
    memset(_rx_buffer, 0x0, BATCH_TRANSMIT_SIZE * 2);
    BSP_AUDIO_In_Out_Transfer(_rx_buffer, _tx_buffer, BATCH_TRANSMIT_SIZE);

    return AUDIO_OK;
}

int AudioClass::write(char* buffer, int length)
{
    if (buffer == NULL || length < 0)
    {
        return -1;
    }

    int copySize = length;
    if (length > Play_Chunk)
    {
        copySize = Play_Chunk;
    }

    memcpy((char*)_play_buffer, buffer, copySize);
    return copySize;
}

int AudioClass::read(char* buffer, int length)
{
    if (buffer == NULL || length < 0)
    {
        return -1;
    }

    int copySize = length;
    if (length > BATCH_TRANSMIT_SIZE * 2)
    {
        copySize = BATCH_TRANSMIT_SIZE * 2;
    }

    memcpy(buffer, _record_buffer, copySize);
    return copySize;
}

int AudioClass::startPlay()
{
    if (audioCallbackFptr != NULL)
    {
        // Write audio binary to Tx buffer
        audioCallbackFptr();
    }

    // Start the audio player
    _audioState = AUDIO_STATE_PLAYING;

    // Set the start pointer of the PCM raw data.
    uint16_t * pStart = (uint16_t *)(_play_buffer + WAVE_HEADER_SIZE);

    // Set the total number of data to be played (count in half-word)
    uint32_t playSize = (uint32_t)(Play_Chunk - WAVE_HEADER_SIZE) / STEREO;

    BSP_AUDIO_OUT_Play(pStart, playSize);

    return AUDIO_OK;
}

/*
** @brief Start playing audio data using underlying codec
** @param audioFile Pointer to audio file
** @param fileSize Size of audio file
** @param channels Number of channels of the audio file
** @retval AUDIO_OK if correct playing, else wrong playing
*/
int AudioClass::startPlay(char *audioFile, int fileSize)
{
    if (audioFile == NULL || fileSize <= WAVE_HEADER_SIZE)
    {
        return AUDIO_ERROR;
    }

    _wavFile = audioFile;
    _audioFileSize = fileSize;

    // Set the current audio pointer position
    _playCursor = (uint16_t *)(_wavFile + WAVE_HEADER_SIZE);

    // Set the total number of data to be played (count in half-word)
    int pcmDataSize = (fileSize - WAVE_HEADER_SIZE) / STEREO;

    // Start the audio player
    _audioState = AUDIO_STATE_PLAYING;
    BSP_AUDIO_OUT_Play((uint16_t *)_playCursor, (uint32_t)(fileSize - WAVE_HEADER_SIZE));

    _audioRemSize = pcmDataSize - DMA_MAX(pcmDataSize);
    _playCursor += DMA_MAX(pcmDataSize);

    return AUDIO_OK;
}

/*
 * @brief stop audio data transmition
*/
void AudioClass::stop()
{
    if (_audioState != AUDIO_STATE_IDLE)
    {
        BSP_AUDIO_STOP();
        _audioState = AUDIO_STATE_IDLE;
    }
}

int AudioClass::getAudioState()
{
    return _audioState;
}

/*
 * @brief compose the WAVE header according to the raw data size
 */
void AudioClass::genericWAVHeader(WaveHeader *hdr, int pcmDataSize, uint32_t sampleRate, uint16_t sampleBitDepth, uint8_t channels)
{
    if (hdr == NULL)
    {
        return;
    }

    memcpy(&hdr->RIFF_marker, "RIFF", 4);
    memcpy(&hdr->filetype_header, "WAVE", 4);
    memcpy(&hdr->format_marker, "fmt ", 4);
    hdr->data_header_length = 16;
    hdr->format_type = 1;
    hdr->number_of_channels = channels;
    hdr->sample_rate = sampleRate;
    hdr->bytes_per_second = sampleRate * channels * sampleBitDepth / 8;
    hdr->bytes_per_frame = channels * sampleBitDepth / 8;
    hdr->bits_per_sample = sampleBitDepth;
    hdr->file_size = pcmDataSize + 36;
    memcpy(&hdr->data_chunk_id, "data", 4);
    hdr->data_chunk_size = pcmDataSize;
}

/*
** Get wave file
*/
char *AudioClass::getWav(int *file_size)
{
    if (_audioState == AUDIO_STATE_RECORDING)
    {
        _audioState = AUDIO_STATE_RECORDING_FINISH;
    }

    int currentSize = _recordCursor - _wavFile;
    *file_size = (int)currentSize;

    // write wave header for this audio file
    WaveHeader hdr;
    genericWAVHeader(&hdr, currentSize - WAVE_HEADER_SIZE, _sampleRate, _sampleBitDepth, _channels);
    memcpy(_wavFile, &hdr, sizeof(WaveHeader));

    return _wavFile;
}

int AudioClass::convertToMono(char *audioFile, int size, int sampleBitLength)
{
    if (sampleBitLength != 16 && sampleBitLength != 24 && sampleBitLength != 32)
    {
        // TODO: log error
        return -1;
    }

    if (audioFile == NULL || size < WAVE_HEADER_SIZE)
    {
        // TODO: log error
        return -1;
    }

    int bytesPerSample = sampleBitLength / 8;
    int curFileSize = 0;

    char *curReader = audioFile + WAVE_HEADER_SIZE + bytesPerSample * 2;
    char *curWriter = audioFile + WAVE_HEADER_SIZE + bytesPerSample;

    if (sampleBitLength == 16)
    {
        while (curReader < audioFile + size)
        {
            *(uint16_t *)curWriter = *((uint16_t *)curReader);

            curWriter += bytesPerSample;
            curReader += bytesPerSample * 2;
        }
    }
    else if (sampleBitLength == 32)
    {
        while (curReader < audioFile + size)
        {
            *(uint32_t *)curWriter = *((uint32_t *)curReader);

            curWriter += bytesPerSample;
            curReader += bytesPerSample * 2;
        }
    }
    else
    {
        while (curReader < audioFile + size)
        {
            memcpy(curWriter, curReader, bytesPerSample);

            curWriter += bytesPerSample;
            curReader += bytesPerSample * 2;
        }
    }

    curFileSize = curWriter - audioFile;

    // re-calculate wave header since the raw data is re-sized from stereo to mono
    WaveHeader hdr;
    genericWAVHeader(&hdr, curFileSize - WAVE_HEADER_SIZE, _sampleRate, _sampleBitDepth, 1);
    memcpy(audioFile, &hdr, sizeof(WaveHeader));

    // clean up the remaining file
    //memset(curWriter, 0, audioFile + size - curWriter);

    return curFileSize;
}

double AudioClass::getRecordedDuration()
{
    if (_wavFile == NULL)
    {
        return 0;
    }

    int pcmDataSize = _recordCursor - _wavFile - WAVE_HEADER_SIZE;

    int bytes_per_second = _sampleRate * _channels * _sampleBitDepth / 8;
    return pcmDataSize / bytes_per_second;
}

int AudioClass::getCurrentSize()
{
    if (_wavFile == NULL)
    {
        return 0;
    }

    return _recordCursor - _wavFile;
}

void AudioClass::attachPlay(callbackFunc func)
{
    audioCallbackFptr = func;
}

void AudioClass::attachRecord(callbackFunc func)
{
    recordCallbackFptr = func;
}

/*------------------------------------------------------------------------------
       Callbacks implementation:
           the callbacks API are defined __weak in the stm32412g_discovery_audio.c file
           and their implementation should be done the user code if they are needed.
           Below some examples of callback implementations.
  ----------------------------------------------------------------------------*/
/**
  * @brief  Manages the full Transfer complete event.
  * @param  None
  * @retval None
  */
void BSP_AUDIO_IN_TransferComplete_CallBack(void)
{
    if (_audioState == AUDIO_STATE_RECORDING)
    {
        int copySize = BATCH_TRANSMIT_SIZE * 2;

        // Copy audio data from codec to user buffer.
        memcpy(_record_buffer, (char *)(_tx_buffer), copySize);

        memset(_rx_buffer, 0x0, BATCH_TRANSMIT_SIZE * 2);
        BSP_AUDIO_In_Out_Transfer(_rx_buffer, _tx_buffer, BATCH_TRANSMIT_SIZE);
        if (recordCallbackFptr != NULL)
        {
            recordCallbackFptr();
        }
    }
}

void BSP_AUDIO_OUT_TransferComplete_CallBack(void)
{
    if (_audioState == AUDIO_STATE_PLAYING)
    {
        if (audioCallbackFptr != NULL)
        {
            audioCallbackFptr();
        }

        BSP_AUDIO_OUT_ChangeBuffer((uint16_t *)_play_buffer, DMA_MAX(Play_Chunk/2));
    }
}


/**
  * @brief  Manages the DMA FIFO error event.
  * @param  None
  * @retval None
  */
void BSP_AUDIO_OUT_Error_CallBack(void)
{
    /* Display message on the LCD screen */
}