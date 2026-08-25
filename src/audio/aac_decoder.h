// Decodificador AAC por software (faad2). El CPU de la consola va sobrado
// para esto: lo caro es el vídeo, y de eso se encarga el hardware.
#pragma once
#include <wut_types.h>
#include <stdint.h>

// Se inicializa con el AudioSpecificConfig que el demuxer saca del esds.
// Devuelve por `outRate`/`outChannels` lo que el propio stream declara, que
// puede diferir del sample entry del MP4 (p.ej. con SBR).
BOOL aac_decoder_open(const uint8_t *asc, uint32_t ascSize,
                      int *outRate, int *outChannels);
void aac_decoder_close(void);
BOOL aac_decoder_ready(void);

// Decodifica un sample AAC. Devuelve puntero a PCM entrelazado de 16 bits
// propiedad del decodificador (válido hasta la siguiente llamada), y el
// número de frames en `outFrames`. NULL si el frame no se pudo decodificar.
const int16_t *aac_decoder_decode(const uint8_t *data, uint32_t size,
                                  uint32_t *outFrames);

int aac_decoder_channels(void);
int aac_decoder_rate(void);
uint32_t aac_decoder_errors(void);
