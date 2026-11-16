/*
 * An effect file (.ffe) for the EnumEffectsInFile tests
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_DINPUT_TEST_EFFECT_FILE_H
#define __WINE_DINPUT_TEST_EFFECT_FILE_H

/* The layout of an effect file, the same for every reader: the DIEFFECT in
 * it has 32-bit pointer fields, which hold the DIEP_* flag of their member
 * when a 'data' chunk for it follows, and 0 when none does. Its effects use
 * object offsets and polar or cartesian directions, as the files games ship
 * do, with the parameters the force feedback tests know the PID reports of. */
struct ffe_chunk
{
    char id[4];
    DWORD size;
};

struct ffe_dieffect
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwDuration;
    DWORD dwSamplePeriod;
    DWORD dwGain;
    DWORD dwTriggerButton;
    DWORD dwTriggerRepeatInterval;
    DWORD cAxes;
    DWORD rgdwAxes;
    DWORD rglDirection;
    DWORD lpEnvelope;
    DWORD cbTypeSpecificParams;
    DWORD lpvTypeSpecificParams;
    DWORD dwStartDelay;
};

struct ffe_effect_header
{
    struct ffe_chunk list;
    char type[4];
    char name[64];
    struct ffe_dieffect effect;
    GUID guid;
    DWORD unknown;
};

struct ffe_spring
{
    struct ffe_effect_header header;
    struct ffe_chunk axes_chunk;
    DWORD axes[2];
    struct ffe_chunk directions_chunk;
    LONG directions[2];
    struct ffe_chunk envelope_chunk;
    DIENVELOPE envelope;
    struct ffe_chunk params_chunk;
    DICONDITION conditions[2];
};

struct ffe_constant
{
    struct ffe_effect_header header;
    struct ffe_chunk axes_chunk;
    DWORD axes[1];
    struct ffe_chunk directions_chunk;
    LONG directions[1];
    struct ffe_chunk params_chunk;
    DICONSTANTFORCE constant;
};

struct ffe_sine
{
    struct ffe_effect_header header;
    struct ffe_chunk axes_chunk;
    DWORD axes[2];
    struct ffe_chunk directions_chunk;
    LONG directions[2];
    struct ffe_chunk envelope_chunk;
    DIENVELOPE envelope;
    struct ffe_chunk params_chunk;
    DIPERIODIC periodic;
};

struct ffe_vendor
{
    struct ffe_effect_header header;
    struct ffe_chunk axes_chunk;
    DWORD axes[1];
};

struct ffe_effects
{
    struct ffe_spring spring;
    struct ffe_constant constant;
    struct ffe_sine sine;
    struct ffe_vendor vendor;
};

struct ffe_file
{
    char riff[4];
    DWORD riff_size;
    char form[4];
    GUID header;
    struct ffe_effects effects;
};

C_ASSERT( sizeof(struct ffe_dieffect) == 56 );
C_ASSERT( sizeof(struct ffe_effect_header) == 152 );
C_ASSERT( sizeof(struct ffe_spring) == 268 );
C_ASSERT( sizeof(struct ffe_constant) == 188 );
C_ASSERT( sizeof(struct ffe_sine) == 236 );
C_ASSERT( sizeof(struct ffe_vendor) == 164 );
C_ASSERT( sizeof(struct ffe_file) == 884 );

#define FFE_EFFECT_HEADER( effect_type, effect_name )                                                \
    .list = {.id = {'L','I','S','T'}, .size = sizeof(effect_type) - sizeof(struct ffe_chunk)},       \
    .type = {'e','f','c','t'}, .name = effect_name, .unknown = 1
#define FFE_DATA( member ) {.id = {'d','a','t','a'}, .size = sizeof(member)}

static const struct ffe_file ffe_file =
{
    .riff = {'R','I','F','F'},
    .riff_size = sizeof(struct ffe_file) - sizeof(struct ffe_chunk),
    .form = {'F','O','R','C'},
    .header = {0x197e775c, 0x34ba, 0x11d3, {0xab, 0xd5, 0x00, 0xc0, 0x4f, 0x8e, 0xc6, 0x27}},
    .effects.spring =
    {
        .header =
        {
            FFE_EFFECT_HEADER( struct ffe_spring, "Spring" ),
            .guid = {0x13541c27, 0x8e33, 0x11d0, {0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35}},
            .effect =
            {
                .dwSize = sizeof(struct ffe_dieffect),
                .dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_POLAR,
                .dwDuration = 1000,
                .dwSamplePeriod = 2000,
                .dwGain = 3000,
                .dwTriggerButton = DIJOFS_BUTTON(0),
                .dwTriggerRepeatInterval = 5000,
                .cAxes = 2,
                .rgdwAxes = DIEP_AXES,
                .rglDirection = DIEP_DIRECTION,
                .lpEnvelope = DIEP_ENVELOPE,
                .cbTypeSpecificParams = sizeof(DICONDITION[2]),
                .lpvTypeSpecificParams = DIEP_TYPESPECIFICPARAMS,
                .dwStartDelay = 6000,
            },
        },
        .axes_chunk = FFE_DATA( DWORD[2] ),
        .axes = {DIJOFS_X, DIJOFS_Z},
        .directions_chunk = FFE_DATA( LONG[2] ),
        .directions = {12000, 0},
        .envelope_chunk = FFE_DATA( DIENVELOPE ),
        .envelope = {.dwSize = sizeof(DIENVELOPE), .dwAttackLevel = 1000, .dwAttackTime = 2000,
                     .dwFadeLevel = 3000, .dwFadeTime = 4000},
        .params_chunk = FFE_DATA( DICONDITION[2] ),
        .conditions =
        {
            {.lOffset = -500, .lPositiveCoefficient = 2000, .lNegativeCoefficient = -3000,
             .dwPositiveSaturation = -4000, .dwNegativeSaturation = -5000, .lDeadBand = 6000},
            {.lOffset = 6000, .lPositiveCoefficient = 5000, .lNegativeCoefficient = -4000,
             .dwPositiveSaturation = 3000, .dwNegativeSaturation = 2000, .lDeadBand = 1000},
        },
    },
    .effects.constant =
    {
        .header =
        {
            FFE_EFFECT_HEADER( struct ffe_constant, "Push" ),
            .guid = {0x13541c20, 0x8e33, 0x11d0, {0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35}},
            .effect =
            {
                .dwSize = sizeof(struct ffe_dieffect),
                .dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN,
                .dwDuration = INFINITE,
                .dwGain = 10000,
                .dwTriggerButton = DIEB_NOTRIGGER,
                .cAxes = 1,
                .rgdwAxes = DIEP_AXES,
                .rglDirection = DIEP_DIRECTION,
                .cbTypeSpecificParams = sizeof(DICONSTANTFORCE),
                .lpvTypeSpecificParams = DIEP_TYPESPECIFICPARAMS,
            },
        },
        .axes_chunk = FFE_DATA( DWORD[1] ),
        .axes = {DIJOFS_X},
        .directions_chunk = FFE_DATA( LONG[1] ),
        .directions = {1},
        .params_chunk = FFE_DATA( DICONSTANTFORCE ),
        .constant = {.lMagnitude = -5000},
    },
    .effects.sine =
    {
        .header =
        {
            FFE_EFFECT_HEADER( struct ffe_sine, "Buffet" ),
            .guid = {0x13541c23, 0x8e33, 0x11d0, {0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35}},
            .effect =
            {
                .dwSize = sizeof(struct ffe_dieffect),
                .dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_POLAR,
                .dwDuration = 1000,
                .dwSamplePeriod = 2000,
                .dwGain = 3000,
                .dwTriggerButton = DIJOFS_BUTTON(0),
                .dwTriggerRepeatInterval = 5000,
                .cAxes = 2,
                .rgdwAxes = DIEP_AXES,
                .rglDirection = DIEP_DIRECTION,
                .lpEnvelope = DIEP_ENVELOPE,
                .cbTypeSpecificParams = sizeof(DIPERIODIC),
                .lpvTypeSpecificParams = DIEP_TYPESPECIFICPARAMS,
                .dwStartDelay = 6000,
            },
        },
        .axes_chunk = FFE_DATA( DWORD[2] ),
        .axes = {DIJOFS_X, DIJOFS_Y},
        .directions_chunk = FFE_DATA( LONG[2] ),
        .directions = {12000, 0},
        .envelope_chunk = FFE_DATA( DIENVELOPE ),
        .envelope = {.dwSize = sizeof(DIENVELOPE), .dwAttackLevel = 1000, .dwAttackTime = 2000,
                     .dwFadeLevel = 3000, .dwFadeTime = 4000},
        .params_chunk = FFE_DATA( DIPERIODIC ),
        .periodic = {.dwMagnitude = 1000, .lOffset = 2000, .dwPhase = 3000, .dwPeriod = 4000},
    },
    .effects.vendor =
    {
        .header =
        {
            FFE_EFFECT_HEADER( struct ffe_vendor, "Vendor" ),
            .guid = {0x2f8b5b1a, 0x6e2d, 0x4c1b, {0x9b, 0x2a, 0x5d, 0x3c, 0x7e, 0x11, 0x42, 0x90}},
            .effect =
            {
                .dwSize = sizeof(struct ffe_dieffect),
                .dwFlags = DIEFF_OBJECTOFFSETS,
                .dwDuration = 1000,
                .dwGain = 5000,
                .dwTriggerButton = DIEB_NOTRIGGER,
                .cAxes = 1,
                .rgdwAxes = DIEP_AXES,
            },
        },
        .axes_chunk = FFE_DATA( DWORD[1] ),
        .axes = {DIJOFS_X},
    },
};

#undef FFE_EFFECT_HEADER
#undef FFE_DATA

#endif /* __WINE_DINPUT_TEST_EFFECT_FILE_H */
