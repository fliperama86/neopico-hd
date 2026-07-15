#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvs_color.h"
#include "mvs_color_model.h"
#include "mvs_color_reference.h"
#include "mvs_effect_lut.h"

enum {
    MVS_FLAG_SHADOW = 1U,
    MVS_FLAG_DARK = 2U,
};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb888_t;

typedef uint8_t (*reference_channel_fn)(const void *context, uint32_t value5, bool dark, bool shadow);

typedef struct {
    uint32_t mismatched_pixels;
    uint32_t max_error[3];
    uint64_t total_error[3];
} comparison_stats_t;

// Memory-safe two-lookup candidates. Tables are indexed by the two effect
// flags plus raw R/G or B fields and include the configured input correction.
typedef struct {
    uint16_t rg[4][1024];
    uint16_t b[4][32];
} split_rgb565_tables_t;

typedef struct {
    uint32_t rg[4][1024];
    uint32_t b[4][32];
} split_rgb888_tables_t;

static uint16_t g_release_normal_lut[MVS_CAPTURE_COLOR_SIZE];
static uint16_t g_analog_normal_lut[MVS_CAPTURE_COLOR_SIZE];
static uint32_t g_raw_by_corrected[MVS_CAPTURE_COLOR_SIZE];
static split_rgb565_tables_t g_mister_split_rgb565;
static split_rgb565_tables_t g_mame_split_rgb565;
static split_rgb888_tables_t g_mister_split_rgb888;
static split_rgb888_tables_t g_mame_split_rgb888;
static mvs_effect_lut_t g_production_effect_lut;
static unsigned g_check_failures;

#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "CHECK FAILED: ");                                                                         \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            g_check_failures++;                                                                                        \
        }                                                                                                              \
    } while (0)

static rgb888_t hstx_rgb565_to_rgb888(uint16_t pixel)
{
    const rgb888_t result = {
        .r = (uint8_t)(((pixel >> 11U) & 0x1FU) << 3U),
        .g = (uint8_t)(((pixel >> 5U) & 0x3FU) << 2U),
        .b = (uint8_t)((pixel & 0x1FU) << 3U),
    };
    return result;
}

static uint32_t scale_5_to_8_linear(uint32_t value)
{
    return (value * 255U + 15U) / 31U;
}

static uint32_t abs_diff_u8(uint8_t a, uint8_t b)
{
    return (a > b) ? (uint32_t)(a - b) : (uint32_t)(b - a);
}

static uint8_t mister_model_channel(const void *context, uint32_t value5, bool dark, bool shadow)
{
    (void)context;
    return mister_reference_channel(value5, dark, shadow);
}

static uint8_t mame_model_channel(const void *context, uint32_t value5, bool dark, bool shadow)
{
    return mame_reference_channel((const mame_reference_t *)context, value5, dark, shadow);
}

static uint8_t selected_reference_channel(const void *context, uint32_t value5, bool dark, bool shadow)
{
#if MVS_EFFECT_MODEL == MVS_EFFECT_MODEL_MISTER
    (void)context;
    return mister_reference_channel(value5, dark, shadow);
#elif MVS_EFFECT_MODEL == MVS_EFFECT_MODEL_MAME
    return mame_reference_channel((const mame_reference_t *)context, value5, dark, shadow);
#else
#error "Unsupported production effect model"
#endif
}

static rgb888_t reference_rgb888(reference_channel_fn channel_fn, const void *context, uint32_t r5, uint32_t g5,
                                 uint32_t b5, uint32_t flags)
{
    const bool shadow = (flags & MVS_FLAG_SHADOW) != 0U;
    const bool dark = (flags & MVS_FLAG_DARK) != 0U;
    const rgb888_t result = {
        .r = channel_fn(context, r5, dark, shadow),
        .g = channel_fn(context, g5, dark, shadow),
        .b = channel_fn(context, b5, dark, shadow),
    };
    return result;
}

static uint16_t reference_pack_rgb565(rgb888_t pixel)
{
    return (uint16_t)(((uint16_t)(pixel.r >> 3U) << 11U) | ((uint16_t)(pixel.g >> 2U) << 5U) |
                      (uint16_t)(pixel.b >> 3U));
}

static uint32_t reference_pack_rgb888(rgb888_t pixel)
{
    return ((uint32_t)pixel.r << 16U) | ((uint32_t)pixel.g << 8U) | pixel.b;
}

static void comparison_add(comparison_stats_t *stats, rgb888_t actual, rgb888_t expected)
{
    const uint32_t error[3] = {
        abs_diff_u8(actual.r, expected.r),
        abs_diff_u8(actual.g, expected.g),
        abs_diff_u8(actual.b, expected.b),
    };
    if (error[0] != 0U || error[1] != 0U || error[2] != 0U) {
        stats->mismatched_pixels++;
    }
    for (uint32_t channel = 0; channel < 3U; channel++) {
        stats->total_error[channel] += error[channel];
        if (error[channel] > stats->max_error[channel]) {
            stats->max_error[channel] = error[channel];
        }
    }
}

static uint32_t normalize_raw_color_idx(uint32_t color_idx)
{
    uint32_t normalized = color_idx ^ (MVS_RAW_COLOR_MASK & MVS_CAPTURE_COLOR_MASK);
#if MVS_REVERSE_15BIT
    normalized = mvs_reverse_15(normalized);
#endif
    return normalized & MVS_CAPTURE_COLOR_MASK;
}

static void generate_split_tables(split_rgb565_tables_t *rgb565_tables, split_rgb888_tables_t *rgb888_tables,
                                  reference_channel_fn channel_fn, const void *context)
{
    for (uint32_t flags = 0; flags < 4U; flags++) {
        const bool shadow = (flags & MVS_FLAG_SHADOW) != 0U;
        const bool dark = (flags & MVS_FLAG_DARK) != 0U;

        for (uint32_t raw_rg = 0; raw_rg < 1024U; raw_rg++) {
            const uint32_t r5 = mvs_correct_5bit((raw_rg >> 5U) & 0x1FU, MVS_INVERT_R, MVS_REVERSE_R);
            const uint32_t g5 = mvs_correct_5bit(raw_rg & 0x1FU, MVS_INVERT_G, MVS_REVERSE_G);
            const uint8_t r8 = channel_fn(context, r5, dark, shadow);
            const uint8_t g8 = channel_fn(context, g5, dark, shadow);
            rgb565_tables->rg[flags][raw_rg] = (uint16_t)(((uint16_t)(r8 >> 3U) << 11U) | ((uint16_t)(g8 >> 2U) << 5U));
            rgb888_tables->rg[flags][raw_rg] = ((uint32_t)r8 << 16U) | ((uint32_t)g8 << 8U);
        }

        for (uint32_t raw_b = 0; raw_b < 32U; raw_b++) {
            const uint32_t b5 = mvs_correct_5bit(raw_b, MVS_INVERT_B, MVS_REVERSE_B);
            const uint8_t b8 = channel_fn(context, b5, dark, shadow);
            rgb565_tables->b[flags][raw_b] = (uint16_t)(b8 >> 3U);
            rgb888_tables->b[flags][raw_b] = b8;
        }
    }
}

static uint16_t split_rgb565_lookup(const split_rgb565_tables_t *tables, uint32_t color_idx, uint32_t flags)
{
    const uint32_t normalized = normalize_raw_color_idx(color_idx);
    return tables->rg[flags & 3U][normalized >> 5U] | tables->b[flags & 3U][normalized & 0x1FU];
}

static uint32_t split_rgb888_lookup(const split_rgb888_tables_t *tables, uint32_t color_idx, uint32_t flags)
{
    const uint32_t normalized = normalize_raw_color_idx(color_idx);
    return tables->rg[flags & 3U][normalized >> 5U] | tables->b[flags & 3U][normalized & 0x1FU];
}

static void print_comparison(const char *label, const comparison_stats_t stats[4])
{
    static const uint32_t display_order[4] = {0U, MVS_FLAG_DARK, MVS_FLAG_SHADOW, MVS_FLAG_DARK | MVS_FLAG_SHADOW};
    static const char *const state_name[4] = {"normal", "shadow", "dark", "dark+shadow"};

    printf("  %s\n", label);
    for (uint32_t order = 0; order < 4U; order++) {
        const uint32_t state = display_order[order];
        printf("    %-11s mismatch %5" PRIu32 "/%u, max R/G/B %3" PRIu32 "/%3" PRIu32 "/%3" PRIu32
               ", mean %.3f/%.3f/%.3f\n",
               state_name[state], stats[state].mismatched_pixels, MVS_CAPTURE_COLOR_SIZE, stats[state].max_error[0],
               stats[state].max_error[1], stats[state].max_error[2],
               (double)stats[state].total_error[0] / MVS_CAPTURE_COLOR_SIZE,
               (double)stats[state].total_error[1] / MVS_CAPTURE_COLOR_SIZE,
               (double)stats[state].total_error[2] / MVS_CAPTURE_COLOR_SIZE);
    }
}

static void generate_release_lut(void)
{
    for (uint32_t color_idx = 0; color_idx < MVS_CAPTURE_COLOR_SIZE; color_idx++) {
        uint32_t r5;
        uint32_t g5;
        uint32_t b5;
        mvs_correct_color_idx(color_idx, &r5, &g5, &b5);
        if (mvs_is_clamped_black(r5, g5, b5)) {
            g_release_normal_lut[color_idx] = 0;
        } else {
            g_release_normal_lut[color_idx] = mvs_pack_rgb565(r5, g5, b5);
        }
    }
}

static uint16_t release_lookup(uint32_t color_idx, uint32_t flags)
{
    (void)flags;
    return g_release_normal_lut[color_idx];
}

static uint64_t hash_lut(const uint16_t *lut, size_t entries)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < entries; i++) {
        hash ^= (uint8_t)(lut[i] & 0xFFU);
        hash *= UINT64_C(1099511628211);
        hash ^= (uint8_t)(lut[i] >> 8U);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int main(void)
{
    bool corrected_seen[MVS_CAPTURE_COLOR_SIZE];
    bool release_rgb565_seen[UINT16_MAX + 1U];
    memset(corrected_seen, 0, sizeof corrected_seen);
    memset(release_rgb565_seen, 0, sizeof release_rgb565_seen);
    memset(g_raw_by_corrected, 0, sizeof g_raw_by_corrected);

    generate_release_lut();

    mame_reference_t mame_reference;
    mame_reference_init(&mame_reference);
    mvs_effect_lut_generate(&g_production_effect_lut);
    generate_split_tables(&g_mister_split_rgb565, &g_mister_split_rgb888, mister_model_channel, NULL);
    generate_split_tables(&g_mame_split_rgb565, &g_mame_split_rgb888, mame_model_channel, &mame_reference);

    uint32_t production_channel_mismatches = 0;
    for (uint32_t flags = 0; flags < 4U; flags++) {
        const bool shadow = (flags & MVS_FLAG_SHADOW) != 0U;
        const bool dark = (flags & MVS_FLAG_DARK) != 0U;
        for (uint32_t value5 = 0; value5 < 32U; value5++) {
            const uint8_t expected = selected_reference_channel(&mame_reference, value5, dark, shadow);
            production_channel_mismatches += (mvs_effect_model_channel(value5, flags) != expected);
        }
    }
    CHECK(production_channel_mismatches == 0U,
          "%s production channel table differs from its independent reference in %" PRIu32 " entries",
          MVS_EFFECT_MODEL_NAME, production_channel_mismatches);
    CHECK(sizeof(g_production_effect_lut) == 8448U, "production split LUT is %zu bytes instead of 8448",
          sizeof(g_production_effect_lut));

    uint32_t analog_channel_mismatches = 0;
    for (uint32_t value5 = 0; value5 < 32U; value5++) {
        analog_channel_mismatches +=
            mvs_color_model_analog_channel(value5) != mame_reference_channel(&mame_reference, value5, false, false);
    }
    CHECK(analog_channel_mismatches == 0U,
          "Analog normal-color channel table differs from its independent reference in %" PRIu32 " entries",
          analog_channel_mismatches);
    CHECK(mvs_color_model_is_valid(MVS_COLOR_MODEL_DIGITAL), "Digital color model is not valid");
    CHECK(mvs_color_model_is_valid(MVS_COLOR_MODEL_ANALOG), "Analog color model is not valid");
    CHECK(!mvs_color_model_is_valid(2U), "out-of-range color model is valid");

    CHECK(mister_reference_channel(31U, false, false) == 255U, "MiSTer normal white endpoint changed");
    CHECK(mister_reference_channel(31U, true, false) == 251U, "MiSTer DARK white endpoint changed");
    CHECK(mister_reference_channel(31U, false, true) == 127U, "MiSTer SHADOW white endpoint changed");
    CHECK(mister_reference_channel(31U, true, true) == 125U, "MiSTer DARK+SHADOW white endpoint changed");
    CHECK(mame_reference_channel(&mame_reference, 31U, false, false) == 255U, "MAME normal white endpoint changed");
    CHECK(mame_reference_channel(&mame_reference, 31U, true, false) == 251U, "MAME DARK white endpoint changed");
    CHECK(mame_reference_channel(&mame_reference, 31U, false, true) == 142U, "MAME SHADOW white endpoint changed");
    CHECK(mame_reference_channel(&mame_reference, 31U, true, true) == 141U, "MAME DARK+SHADOW white endpoint changed");

    uint32_t corrected_duplicates = 0;
    uint32_t release_pack_code_mismatches = 0;
    uint32_t release_pack_format_mismatches = 0;
    uint32_t clamped_cube_entries = 0;
    uint32_t release_nonblack_mapped_to_black = 0;
    uint32_t release_lut_code_mismatches = 0;
    uint32_t digital_model_mismatches = 0;
    uint32_t analog_model_mismatches = 0;
    uint32_t unique_release_normal_outputs = 0;
    uint64_t linear_error_r = 0;
    uint64_t linear_error_g = 0;
    uint64_t linear_error_b = 0;
    uint32_t linear_max_error_r = 0;
    uint32_t linear_max_error_g = 0;
    uint32_t linear_max_error_b = 0;

    for (uint32_t color_idx = 0; color_idx < MVS_CAPTURE_COLOR_SIZE; color_idx++) {
        uint32_t r5;
        uint32_t g5;
        uint32_t b5;
        mvs_correct_color_idx(color_idx, &r5, &g5, &b5);

        const uint32_t corrected_idx = (r5 << 10U) | (g5 << 5U) | b5;
        if (corrected_seen[corrected_idx]) {
            corrected_duplicates++;
        } else {
            corrected_seen[corrected_idx] = true;
            g_raw_by_corrected[corrected_idx] = color_idx;
        }

        const uint16_t direct = mvs_pack_rgb565(r5, g5, b5);
        const uint16_t digital_model = mvs_color_model_pack_rgb565(MVS_COLOR_MODEL_DIGITAL, r5, g5, b5);
        const uint16_t analog_model = mvs_color_model_pack_rgb565(MVS_COLOR_MODEL_ANALOG, r5, g5, b5);
        const rgb888_t analog_reference = reference_rgb888(mame_model_channel, &mame_reference, r5, g5, b5, 0U);
        const uint16_t analog_expected = reference_pack_rgb565(analog_reference);
        g_analog_normal_lut[color_idx] = analog_model;
        digital_model_mismatches += digital_model != direct;
        analog_model_mismatches += analog_model != analog_expected;
        const uint32_t direct_r5 = (direct >> 11U) & 0x1FU;
        const uint32_t direct_g6 = (direct >> 5U) & 0x3FU;
        const uint32_t direct_b5 = direct & 0x1FU;
        const uint32_t expected_g6 = (g5 << 1U) | (g5 >> 4U);
        if (direct_r5 != r5 || (direct_g6 >> 1U) != g5 || direct_b5 != b5) {
            release_pack_code_mismatches++;
        }
        if (direct_r5 != r5 || direct_g6 != expected_g6 || direct_b5 != b5) {
            release_pack_format_mismatches++;
        }

        const bool clamped = mvs_is_clamped_black(r5, g5, b5);
        if (clamped) {
            clamped_cube_entries++;
        }
        if ((r5 != 0U || g5 != 0U || b5 != 0U) && g_release_normal_lut[color_idx] == 0U) {
            release_nonblack_mapped_to_black++;
        }
        const uint16_t release_normal = g_release_normal_lut[color_idx];
        const uint32_t release_out_r5 = (release_normal >> 11U) & 0x1FU;
        const uint32_t release_out_g5 = ((release_normal >> 5U) & 0x3FU) >> 1U;
        const uint32_t release_out_b5 = release_normal & 0x1FU;
        if (release_out_r5 != r5 || release_out_g5 != g5 || release_out_b5 != b5) {
            release_lut_code_mismatches++;
        }

        if (!release_rgb565_seen[release_normal]) {
            release_rgb565_seen[release_normal] = true;
            unique_release_normal_outputs++;
        }

        const rgb888_t wire = hstx_rgb565_to_rgb888(direct);
        const uint8_t target_r = (uint8_t)scale_5_to_8_linear(r5);
        const uint8_t target_g = (uint8_t)scale_5_to_8_linear(g5);
        const uint8_t target_b = (uint8_t)scale_5_to_8_linear(b5);
        const uint32_t error_r = abs_diff_u8(wire.r, target_r);
        const uint32_t error_g = abs_diff_u8(wire.g, target_g);
        const uint32_t error_b = abs_diff_u8(wire.b, target_b);
        linear_error_r += error_r;
        linear_error_g += error_g;
        linear_error_b += error_b;
        if (error_r > linear_max_error_r) {
            linear_max_error_r = error_r;
        }
        if (error_g > linear_max_error_g) {
            linear_max_error_g = error_g;
        }
        if (error_b > linear_max_error_b) {
            linear_max_error_b = error_b;
        }
    }

    CHECK(corrected_duplicates == 0U, "input correction is not one-to-one: %" PRIu32 " duplicates",
          corrected_duplicates);
    CHECK(release_pack_code_mismatches == 0U, "default RGB555 to RGB565 pack lost source codes: %" PRIu32 " failures",
          release_pack_code_mismatches);
    CHECK(release_pack_format_mismatches == 0U,
          "default RGB555 to RGB565 pack violates standard green expansion: %" PRIu32 " failures",
          release_pack_format_mismatches);
    CHECK(clamped_cube_entries == 0U, "accuracy build still clamps %" PRIu32 " RGB555 colors", clamped_cube_entries);
    CHECK(release_nonblack_mapped_to_black == 0U, "default LUT maps %" PRIu32 " nonblack colors to black",
          release_nonblack_mapped_to_black);
    CHECK(release_lut_code_mismatches == 0U, "default LUT loses %" PRIu32 " RGB555 source codes",
          release_lut_code_mismatches);
    CHECK(digital_model_mismatches == 0U,
          "Digital color model differs from the stable RGB555 mapping in %" PRIu32 " cases", digital_model_mismatches);
    CHECK(analog_model_mismatches == 0U,
          "Analog color model differs from the independent normal-state DAC reference in %" PRIu32 " cases",
          analog_model_mismatches);
    CHECK(unique_release_normal_outputs == MVS_CAPTURE_COLOR_SIZE,
          "default LUT has %" PRIu32 " unique outputs instead of %u", unique_release_normal_outputs,
          MVS_CAPTURE_COLOR_SIZE);

    uint64_t state_cases = 0;
    uint32_t release_effect_cases_unchanged = 0;
    uint32_t production_dark_differs_from_normal = 0;
    uint32_t production_shadow_differs_from_normal = 0;
    uint32_t production_both_differs_from_normal = 0;
    uint32_t production_shadow_both_differences = 0;

    for (uint32_t color_idx = 0; color_idx < MVS_CAPTURE_COLOR_SIZE; color_idx++) {
        const uint16_t normal = mvs_effect_lut_lookup_color(&g_production_effect_lut, color_idx, 0U);
        const uint16_t release_normal = release_lookup(color_idx, 0U);
        const uint16_t dark = mvs_effect_lut_lookup_color(&g_production_effect_lut, color_idx, MVS_FLAG_DARK);
        const uint16_t shadow = mvs_effect_lut_lookup_color(&g_production_effect_lut, color_idx, MVS_FLAG_SHADOW);
        const uint16_t both =
            mvs_effect_lut_lookup_color(&g_production_effect_lut, color_idx, MVS_FLAG_DARK | MVS_FLAG_SHADOW);

        production_dark_differs_from_normal += (dark != normal);
        production_shadow_differs_from_normal += (shadow != normal);
        production_both_differs_from_normal += (both != normal);
        production_shadow_both_differences += (shadow != both);

        for (uint32_t flags = 0; flags < 4U; flags++) {
            const uint16_t release = release_lookup(color_idx, flags);
            state_cases++;
            if (flags != 0U && release == release_normal) {
                release_effect_cases_unchanged++;
            }
        }
    }

    CHECK(state_cases == (uint64_t)MVS_CAPTURE_COLOR_SIZE * 4U,
          "did not exercise every color/effect combination: %" PRIu64, state_cases);
    CHECK(production_dark_differs_from_normal != 0U, "%s DARK state collapsed to normal", MVS_EFFECT_MODEL_NAME);
    CHECK(production_shadow_differs_from_normal != 0U, "%s SHADOW state collapsed to normal", MVS_EFFECT_MODEL_NAME);
    CHECK(production_both_differs_from_normal != 0U, "%s DARK+SHADOW state collapsed to normal", MVS_EFFECT_MODEL_NAME);
    CHECK(production_shadow_both_differences != 0U, "%s SHADOW and DARK+SHADOW states collapsed together",
          MVS_EFFECT_MODEL_NAME);

    comparison_stats_t mister_vs_mame[4] = {0};
    comparison_stats_t mister_rgb565_transport[4] = {0};
    comparison_stats_t mame_rgb565_transport[4] = {0};
    uint32_t mister_split_rgb565_mismatches = 0;
    uint32_t mame_split_rgb565_mismatches = 0;
    uint32_t mister_split_rgb888_mismatches = 0;
    uint32_t mame_split_rgb888_mismatches = 0;
    uint32_t production_split_rgb565_mismatches = 0;
    uint32_t production_raw_lookup_mismatches = 0;

    for (uint32_t color_idx = 0; color_idx < MVS_CAPTURE_COLOR_SIZE; color_idx++) {
        uint32_t r5;
        uint32_t g5;
        uint32_t b5;
        mvs_correct_color_idx(color_idx, &r5, &g5, &b5);

        for (uint32_t flags = 0; flags < 4U; flags++) {
            const rgb888_t mister_exact = reference_rgb888(mister_model_channel, NULL, r5, g5, b5, flags);
            const rgb888_t mame_exact = reference_rgb888(mame_model_channel, &mame_reference, r5, g5, b5, flags);
            const uint16_t mister_expected_rgb565 = reference_pack_rgb565(mister_exact);
            const uint16_t mame_expected_rgb565 = reference_pack_rgb565(mame_exact);
            const uint16_t mister_split_rgb565 = split_rgb565_lookup(&g_mister_split_rgb565, color_idx, flags);
            const uint16_t mame_split_rgb565 = split_rgb565_lookup(&g_mame_split_rgb565, color_idx, flags);
            const uint32_t mister_split_rgb888 = split_rgb888_lookup(&g_mister_split_rgb888, color_idx, flags);
            const uint32_t mame_split_rgb888 = split_rgb888_lookup(&g_mame_split_rgb888, color_idx, flags);
            const rgb888_t production_exact =
                reference_rgb888(selected_reference_channel, &mame_reference, r5, g5, b5, flags);
            const uint16_t production_expected_rgb565 = reference_pack_rgb565(production_exact);
            const uint16_t production_split_rgb565 =
                mvs_effect_lut_lookup_color(&g_production_effect_lut, color_idx, flags);
            const uint32_t synthesized_raw = (color_idx << 2U) | (flags << 17U);
            const uint16_t production_raw_rgb565 = mvs_effect_lut_lookup_raw(&g_production_effect_lut, synthesized_raw);

            mister_split_rgb565_mismatches += (mister_split_rgb565 != mister_expected_rgb565);
            mame_split_rgb565_mismatches += (mame_split_rgb565 != mame_expected_rgb565);
            mister_split_rgb888_mismatches += (mister_split_rgb888 != reference_pack_rgb888(mister_exact));
            mame_split_rgb888_mismatches += (mame_split_rgb888 != reference_pack_rgb888(mame_exact));
            production_split_rgb565_mismatches += (production_split_rgb565 != production_expected_rgb565);
            production_raw_lookup_mismatches += (production_raw_rgb565 != production_expected_rgb565);

            comparison_add(&mister_vs_mame[flags], mister_exact, mame_exact);
            comparison_add(&mister_rgb565_transport[flags], hstx_rgb565_to_rgb888(mister_split_rgb565), mister_exact);
            comparison_add(&mame_rgb565_transport[flags], hstx_rgb565_to_rgb888(mame_split_rgb565), mame_exact);
        }
    }

    CHECK(mister_split_rgb565_mismatches == 0U,
          "MiSTer split RGB565 candidate differs from direct model in %" PRIu32 " cases",
          mister_split_rgb565_mismatches);
    CHECK(mame_split_rgb565_mismatches == 0U,
          "MAME split RGB565 candidate differs from direct model in %" PRIu32 " cases", mame_split_rgb565_mismatches);
    CHECK(mister_split_rgb888_mismatches == 0U,
          "MiSTer split RGB888 candidate differs from direct model in %" PRIu32 " cases",
          mister_split_rgb888_mismatches);
    CHECK(mame_split_rgb888_mismatches == 0U,
          "MAME split RGB888 candidate differs from direct model in %" PRIu32 " cases", mame_split_rgb888_mismatches);
    CHECK(production_split_rgb565_mismatches == 0U,
          "%s production split RGB565 differs from its direct reference in %" PRIu32 " cases", MVS_EFFECT_MODEL_NAME,
          production_split_rgb565_mismatches);
    CHECK(production_raw_lookup_mismatches == 0U,
          "%s production raw-word lookup differs from its direct reference in %" PRIu32 " cases", MVS_EFFECT_MODEL_NAME,
          production_raw_lookup_mismatches);

    uint32_t neutral_green_bias_levels = 0;
    for (uint32_t level = 0; level < 32U; level++) {
        const uint32_t corrected_idx = (level << 10U) | (level << 5U) | level;
        const uint32_t raw_idx = g_raw_by_corrected[corrected_idx];
        uint32_t r5;
        uint32_t g5;
        uint32_t b5;
        mvs_correct_color_idx(raw_idx, &r5, &g5, &b5);
        const rgb888_t wire = hstx_rgb565_to_rgb888(mvs_pack_rgb565(r5, g5, b5));
        if (wire.r != wire.g || wire.g != wire.b) {
            neutral_green_bias_levels++;
        }
    }

    const uint32_t white_raw = g_raw_by_corrected[0x7FFFU];
    const rgb888_t release_white = hstx_rgb565_to_rgb888(release_lookup(white_raw, 0U));
    const rgb888_t release_dark_white = hstx_rgb565_to_rgb888(release_lookup(white_raw, MVS_FLAG_DARK));
    rgb888_t production_white[4];
    rgb888_t mister_white[4];
    rgb888_t mame_white[4];
    for (uint32_t flags = 0; flags < 4U; flags++) {
        production_white[flags] =
            hstx_rgb565_to_rgb888(mvs_effect_lut_lookup_color(&g_production_effect_lut, white_raw, flags));
        mister_white[flags] = reference_rgb888(mister_model_channel, NULL, 31U, 31U, 31U, flags);
        mame_white[flags] = reference_rgb888(mame_model_channel, &mame_reference, 31U, 31U, 31U, flags);
    }

    printf("MVS exhaustive color characterization\n");
    printf("  Production effect model:               %s\n", MVS_EFFECT_MODEL_NAME);
    printf("  Production split LUT bytes:             %zu\n", sizeof(g_production_effect_lut));
    printf("  Production channel mismatches:          %" PRIu32 "\n", production_channel_mismatches);
    printf("  Production split/raw mismatches:        %" PRIu32 "/%" PRIu32 "\n", production_split_rgb565_mismatches,
           production_raw_lookup_mismatches);
    printf("  Raw RGB555 colors tested:              %u\n", MVS_CAPTURE_COLOR_SIZE);
    printf("  Color/effect combinations tested:      %" PRIu64 "\n", state_cases);
    printf("  Input correction duplicates:           %" PRIu32 "\n", corrected_duplicates);
    printf("  Default pack source-code mismatches:   %" PRIu32 "\n", release_pack_code_mismatches);
    printf("  Default pack RGB565-format mismatches: %" PRIu32 "\n", release_pack_format_mismatches);
    printf("\nDefault RGB555 LUT\n");
    printf("  Clamp threshold:                       %d\n", MVS_BLACK_LEVEL_CLAMP);
    printf("  Colors inside clamped cube:            %" PRIu32 "\n", clamped_cube_entries);
    printf("  Default nonblack colors mapped black:  %" PRIu32 "\n", release_nonblack_mapped_to_black);
    printf("  Default LUT source-code mismatches:    %" PRIu32 "\n", release_lut_code_mismatches);
    printf("  Digital model mismatches:              %" PRIu32 "\n", digital_model_mismatches);
    printf("  Analog model mismatches:               %" PRIu32 "\n", analog_model_mismatches);
    printf("  Analog channel-table mismatches:       %" PRIu32 "\n", analog_channel_mismatches);
    printf("  Unique default RGB565 outputs:         %" PRIu32 " / %u\n", unique_release_normal_outputs,
           MVS_CAPTURE_COLOR_SIZE);
    printf("  Effect-flag cases emitted unchanged:   %" PRIu32 " / %u\n", release_effect_cases_unchanged,
           MVS_CAPTURE_COLOR_SIZE * 3U);

    printf("\nSelected four-state split LUT\n");
    printf("  DARK colors differing from normal:     %" PRIu32 "\n", production_dark_differs_from_normal);
    printf("  SHADOW colors differing from normal:   %" PRIu32 "\n", production_shadow_differs_from_normal);
    printf("  BOTH colors differing from normal:     %" PRIu32 "\n", production_both_differs_from_normal);
    printf("  SHADOW versus BOTH differences:        %" PRIu32 "\n", production_shadow_both_differences);
    printf("\nCurrent RGB565 HSTX mapping\n");
    printf("  Release normal white:                  (%u,%u,%u)\n", release_white.r, release_white.g, release_white.b);
    printf("  Release DARK white:                    (%u,%u,%u)\n", release_dark_white.r, release_dark_white.g,
           release_dark_white.b);
    printf("  Production normal white:               (%u,%u,%u)\n", production_white[0].r, production_white[0].g,
           production_white[0].b);
    printf("  Production DARK white:                 (%u,%u,%u)\n", production_white[MVS_FLAG_DARK].r,
           production_white[MVS_FLAG_DARK].g, production_white[MVS_FLAG_DARK].b);
    printf("  Production SHADOW white:               (%u,%u,%u)\n", production_white[MVS_FLAG_SHADOW].r,
           production_white[MVS_FLAG_SHADOW].g, production_white[MVS_FLAG_SHADOW].b);
    printf("  Production DARK+SHADOW white:          (%u,%u,%u)\n", production_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].r,
           production_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].g, production_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].b);
    printf("  Neutral levels with unequal R/G/B:     %" PRIu32 " / 32\n", neutral_green_bias_levels);
    printf("  Max error vs round(x*255/31), R/G/B:   %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n", linear_max_error_r,
           linear_max_error_g, linear_max_error_b);
    printf("  Mean error vs linear mapping, R/G/B:   %.3f/%.3f/%.3f\n", (double)linear_error_r / MVS_CAPTURE_COLOR_SIZE,
           (double)linear_error_g / MVS_CAPTURE_COLOR_SIZE, (double)linear_error_b / MVS_CAPTURE_COLOR_SIZE);

    printf("\nPinned four-state reference endpoints\n");
    printf("  State        MiSTer RGB888   MAME RGB888\n");
    printf("  Normal       (%3u,%3u,%3u)   (%3u,%3u,%3u)\n", mister_white[0].r, mister_white[0].g, mister_white[0].b,
           mame_white[0].r, mame_white[0].g, mame_white[0].b);
    printf("  DARK         (%3u,%3u,%3u)   (%3u,%3u,%3u)\n", mister_white[MVS_FLAG_DARK].r,
           mister_white[MVS_FLAG_DARK].g, mister_white[MVS_FLAG_DARK].b, mame_white[MVS_FLAG_DARK].r,
           mame_white[MVS_FLAG_DARK].g, mame_white[MVS_FLAG_DARK].b);
    printf("  SHADOW       (%3u,%3u,%3u)   (%3u,%3u,%3u)\n", mister_white[MVS_FLAG_SHADOW].r,
           mister_white[MVS_FLAG_SHADOW].g, mister_white[MVS_FLAG_SHADOW].b, mame_white[MVS_FLAG_SHADOW].r,
           mame_white[MVS_FLAG_SHADOW].g, mame_white[MVS_FLAG_SHADOW].b);
    printf("  DARK+SHADOW  (%3u,%3u,%3u)   (%3u,%3u,%3u)\n", mister_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].r,
           mister_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].g, mister_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].b,
           mame_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].r, mame_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].g,
           mame_white[MVS_FLAG_DARK | MVS_FLAG_SHADOW].b);

    printf("\nExhaustive reference comparisons (8-bit component error)\n");
    print_comparison("MiSTer vs MAME", mister_vs_mame);
    print_comparison("Exact MiSTer packed through current RGB565/HSTX", mister_rgb565_transport);
    print_comparison("Exact MAME packed through current RGB565/HSTX", mame_rgb565_transport);

    printf("\nEffect LUT memory (one compiled reference profile)\n");
    printf("  Production split RGB565:               %zu bytes\n", sizeof(g_production_effect_lut));
    printf("  Flat four-state RGB565:                %u bytes\n",
           MVS_CAPTURE_COLOR_SIZE * 4U * (unsigned)sizeof(uint16_t));
    printf("  Flat four-state packed RGB888:         %u bytes\n", MVS_CAPTURE_COLOR_SIZE * 4U * 3U);
    printf("  Split R/G + B RGB565, two lookups:     %zu bytes\n", sizeof(split_rgb565_tables_t));
    printf("  Split R/G + B RGB888, two lookups:     %zu bytes\n", sizeof(split_rgb888_tables_t));
    printf("  Three channel RGB565 tables:           %u bytes\n", 3U * 4U * 32U * (unsigned)sizeof(uint16_t));
    printf("  Split RGB565 exactness, MiSTer/MAME:   %" PRIu32 "/%" PRIu32 " mismatches\n",
           mister_split_rgb565_mismatches, mame_split_rgb565_mismatches);
    printf("  Split RGB888 exactness, MiSTer/MAME:   %" PRIu32 "/%" PRIu32 " mismatches\n",
           mister_split_rgb888_mismatches, mame_split_rgb888_mismatches);

    printf("\nDeterministic LUT hashes (FNV-1a 64)\n");
    printf("  Default normal:  0x%016" PRIx64 "\n", hash_lut(g_release_normal_lut, MVS_CAPTURE_COLOR_SIZE));
    printf("  Analog normal:   0x%016" PRIx64 "\n", hash_lut(g_analog_normal_lut, MVS_CAPTURE_COLOR_SIZE));
    printf("  Production RG:   0x%016" PRIx64 "\n", hash_lut(g_production_effect_lut.rg, MVS_EFFECT_RG_TABLE_ENTRIES));
    printf("  Production B:    0x%016" PRIx64 "\n", hash_lut(g_production_effect_lut.b, MVS_EFFECT_B_TABLE_ENTRIES));

    if (g_check_failures != 0U) {
        fprintf(stderr, "\nFAIL: %u exhaustive structural checks failed.\n", g_check_failures);
        return EXIT_FAILURE;
    }

    printf("\nPASS: all exhaustive structural checks completed. Reported accuracy deviations are characterization "
           "results.\n");
    return EXIT_SUCCESS;
}
