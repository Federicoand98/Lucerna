#ifndef COMMON_TONEMAP_GLSL
#define COMMON_TONEMAP_GLSL

// Gli operatori di tonemap. Tutti prendono radianza lineare con primari sRGB/Rec.709 (D65), gia'
// moltiplicata per l'esposizione, e restituiscono valori lineari per il display in [0,1]:
// l'encoding sRGB lo fa chi chiama. L'ordine combacia con ToneMapper in RenderSettings.h.
const int TONEMAP_NONE = 0;
const int TONEMAP_ACES = 1;
const int TONEMAP_ACES_FITTED = 2;
const int TONEMAP_AGX = 3;
const int TONEMAP_NEUTRAL = 4;
const int TONEMAP_REINHARD = 5;
const int TONEMAP_HABLE = 6;

// ---------------------------------------------------------------------------------------------
// ACES 1.0.3: la RRT e l'ODT "sRGB 100 nits dim" della reference CTL dell'Academy
// (aces-dev, rrt/RRT.ctl e odt/sRGB/ODT.Academy.sRGB_100nits_dim.ctl), senza approssimazioni.
// Le matrici sono calcolate dai primari come fanno le utility CTL (Bradford per D65 <-> D60).
// Una mat3 costruita per righe e usata come v * M da' M_righe * v.

const mat3 SRGB_2_AP0 = mat3(0.4396329819, 0.3829886982, 0.1773783199,
    0.0897764430, 0.8134394287, 0.0967841283,
    0.0175411704, 0.1115465533, 0.8709122763);
const mat3 AP0_2_AP1 = mat3(1.4514393161, -0.2365107469, -0.2149285693,
    -0.0765537734, 1.1762296998, -0.0996759264,
    0.0083161484, -0.0060324498, 0.9977163014);
const mat3 AP1_2_AP0 = mat3(0.6954522414, 0.1406786965, 0.1638690622,
    0.0447945634, 0.8596711185, 0.0955343182,
    -0.0055258826, 0.0040252103, 1.0015006723);
const mat3 AP1_2_XYZ = mat3(0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003);
const mat3 XYZ_2_AP1 = mat3(1.6410233797, -0.3248032942, -0.2364246952,
    -0.6636628587, 1.6153315917, 0.0167563477,
    0.0117218943, -0.0082844420, 0.9883948585);
// XYZ (bianco D60) -> sRGB lineare, con l'adattamento cromatico D60 -> D65 dentro
const mat3 XYZ_D60_2_SRGB = mat3(3.2097129271, -1.5575168818, -0.4958277673,
    -0.9709872055, 1.8851730313, 0.0394856503,
    0.0597168695, -0.2100830244, 1.1431058632);
const vec3 AP1_RGB2Y = vec3(0.2722287168, 0.6740817658, 0.0536895174);

// calc_sat_adjust_matrix della CTL, scritta come mix verso la luminanza AP1
vec3 AcesSaturate(vec3 rgb, float sat) {
    return mix(vec3(dot(AP1_RGB2Y, rgb)), rgb, sat);
}

// Un tratto di B-spline quadratica in log10, su tre coefficienti consecutivi
float AcesSplineSegment(float c0, float c1, float c2, float t) {
    return t * t * (0.5 * c0 - c1 + 0.5 * c2) + t * (c1 - c0) + 0.5 * (c0 + c1);
}

const float ACES_HALF_MIN = 5.96046448e-8;
const float C5_LOW[6] = float[](-4.0, -4.0, -3.1573765773, -0.4852499958, 1.8477324706, 1.8477324706);
const float C5_HIGH[6] = float[](-0.7185482425, 2.0810307172, 3.6681241237, 4.0, 4.0, 4.0);

// La tonescale della RRT: log10(0.18 * 2^-15) -> -4, log10(0.18) -> log10(4.8), log10(0.18 * 2^18) -> 4
float AcesSplineC5(float x) {
    const float logMinX = log(0.18 * exp2(-15.0)) / log(10.0);
    const float logMidX = log(0.18) / log(10.0);
    const float logMaxX = log(0.18 * exp2(18.0)) / log(10.0);
    float logx = log(max(x, ACES_HALF_MIN)) / log(10.0);
    float logy;
    if(logx <= logMinX) {
        logy = -4.0; // slopeLow = 0
    } else if(logx < logMidX) {
        float k = 3.0 * (logx - logMinX) / (logMidX - logMinX);
        int j = int(k);
        logy = AcesSplineSegment(C5_LOW[j], C5_LOW[j + 1], C5_LOW[j + 2], k - float(j));
    } else if(logx < logMaxX) {
        float k = 3.0 * (logx - logMidX) / (logMaxX - logMidX);
        int j = int(k);
        logy = AcesSplineSegment(C5_HIGH[j], C5_HIGH[j + 1], C5_HIGH[j + 2], k - float(j));
    } else {
        logy = 4.0; // slopeHigh = 0
    }
    return pow(10.0, logy);
}

const float C9_LOW[10] = float[](-1.6989700043, -1.6989700043, -1.4779, -1.2291, -0.8648, -0.448, 0.00518, 0.4511080334, 0.9113744414, 0.9113744414);
const float C9_HIGH[10] = float[](0.5154386965, 0.8470437783, 1.1358, 1.3802, 1.5197, 1.5985, 1.6467, 1.6746091357, 1.687873339, 1.687873339);

// La tonescale dell'ODT a 48 nit. I punti in x sono AcesSplineC5 di 0.18 * 2^-6.5, 0.18 e
// 0.18 * 2^6.5, calcolati una volta (sono costanti della CTL)
float AcesSplineC9(float x) {
    const float logMinX = log(0.0028798932) / log(10.0);
    const float logMidX = log(4.8) / log(10.0);
    const float logMaxX = log(1005.7193595080) / log(10.0);
    const float logMinY = log(0.02) / log(10.0);
    const float logMaxY = log(48.0) / log(10.0);
    float logx = log(max(x, ACES_HALF_MIN)) / log(10.0);
    float logy;
    if(logx <= logMinX) {
        logy = logMinY; // slopeLow = 0
    } else if(logx < logMidX) {
        float k = 7.0 * (logx - logMinX) / (logMidX - logMinX);
        int j = int(k);
        logy = AcesSplineSegment(C9_LOW[j], C9_LOW[j + 1], C9_LOW[j + 2], k - float(j));
    } else if(logx < logMaxX) {
        float k = 7.0 * (logx - logMidX) / (logMaxX - logMidX);
        int j = int(k);
        logy = AcesSplineSegment(C9_HIGH[j], C9_HIGH[j + 1], C9_HIGH[j + 2], k - float(j));
    } else {
        logy = logx * 0.04 + (logMaxY - 0.04 * logMaxX); // slopeHigh = 0.04
    }
    return pow(10.0, logy);
}

float AcesSaturation(vec3 rgb) {
    float hi = max(rgb.r, max(rgb.g, rgb.b));
    float lo = min(rgb.r, min(rgb.g, rgb.b));
    return (max(hi, 1e-5) - max(lo, 1e-5)) / max(hi, 1e-2);
}

// rgb_2_yc: luminanza che tiene conto della croma, con ycRadiusWeight = 1.75
float AcesYc(vec3 rgb) {
    float chroma = sqrt(max(rgb.b * (rgb.b - rgb.g) + rgb.g * (rgb.g - rgb.r) + rgb.r * (rgb.r - rgb.b), 0.0));
    return (rgb.r + rgb.g + rgb.b + 1.75 * chroma) / 3.0;
}

float AcesSigmoidShaper(float x) {
    float t = max(1.0 - abs(x / 2.0), 0.0);
    return (1.0 + sign(x) * (1.0 - t * t)) / 2.0;
}

float AcesGlow(float yc, float gain, float mid) {
    if(yc <= 2.0 / 3.0 * mid) return gain;
    if(yc >= 2.0 * mid) return 0.0;
    return gain * (mid / yc - 0.5);
}

// Tinta in gradi [0, 360), centrata sul rosso; un grigio vale 0 (la CTL da' NaN e poi 0 di peso)
float AcesHue(vec3 rgb) {
    if(rgb.r == rgb.g && rgb.g == rgb.b) return 0.0;
    float hue = degrees(atan(sqrt(3.0) * (rgb.g - rgb.b), 2.0 * rgb.r - rgb.g - rgb.b));
    return hue < 0.0 ? hue + 360.0 : hue;
}

// cubic_basis_shaper: una B-spline cubica larga w centrata in 0, che vale 1 al centro
float AcesCubicBasis(float x, float w) {
    if(x <= -w / 2.0 || x >= w / 2.0) return 0.0;
    float k = (x + w / 2.0) * 4.0 / w;
    int j = int(k);
    float t = k - float(j);
    vec4 m = vec4(t * t * t, t * t, t, 1.0);
    float y;
    if(j == 3)      y = dot(m, vec4(-1.0, 3.0, -3.0, 1.0) / 6.0);
    else if(j == 2) y = dot(m, vec4(3.0, -6.0, 0.0, 4.0) / 6.0);
    else if(j == 1) y = dot(m, vec4(-3.0, 3.0, 3.0, 1.0) / 6.0);
    else            y = dot(m, vec4(1.0, 0.0, 0.0, 0.0) / 6.0);
    return y * 1.5;
}

// ACES2065-1 (AP0) -> OCES
vec3 AcesRRT(vec3 aces) {
    // glow: schiarisce un po' i toni scuri e saturi
    float saturation = AcesSaturation(aces);
    float s = AcesSigmoidShaper((saturation - 0.4) / 0.2);
    aces *= 1.0 + AcesGlow(AcesYc(aces), 0.05 * s, 0.08);

    // red modifier: tiene sotto controllo i rossi molto saturi
    float hue = AcesHue(aces);
    float centered = hue > 180.0 ? hue - 360.0 : hue;
    float weight = AcesCubicBasis(centered, 135.0);
    aces.r += weight * saturation * (0.03 - aces.r) * (1.0 - 0.82);

    vec3 rgb = clamp(max(aces, 0.0) * AP0_2_AP1, 0.0, 65504.0);
    rgb = AcesSaturate(rgb, 0.96);
    rgb = vec3(AcesSplineC5(rgb.r), AcesSplineC5(rgb.g), AcesSplineC5(rgb.b));
    return rgb * AP1_2_AP0;
}

// OCES -> sRGB lineare per un monitor da 100 nit in ambiente poco illuminato
vec3 AcesODTsRGB(vec3 oces) {
    vec3 rgb = oces * AP0_2_AP1;
    rgb = vec3(AcesSplineC9(rgb.r), AcesSplineC9(rgb.g), AcesSplineC9(rgb.b));
    vec3 cv = (rgb - 0.02) / (48.0 - 0.02); // Y_2_linCV, CINEMA_WHITE 48, CINEMA_BLACK 0.02

    // darkSurround_to_dimSurround: gamma 0.9811 sulla sola Y, a cromaticita' fissa
    vec3 XYZ = cv * AP1_2_XYZ;
    float sum = XYZ.x + XYZ.y + XYZ.z;
    vec2 xy = XYZ.xy / (sum == 0.0 ? 1e-10 : sum);
    float Y = pow(max(XYZ.y, 0.0), 0.9811);
    XYZ = vec3(xy.x * Y / max(xy.y, 1e-10), Y, (1.0 - xy.x - xy.y) * Y / max(xy.y, 1e-10));
    cv = XYZ * XYZ_2_AP1;

    cv = AcesSaturate(cv, 0.93);
    return clamp(cv * AP1_2_XYZ * XYZ_D60_2_SRGB, 0.0, 1.0);
}

vec3 ToneMapACES(vec3 rgb) {
    return AcesODTsRGB(AcesRRT(rgb * SRGB_2_AP0));
}

// ---------------------------------------------------------------------------------------------
// ACES fitted: il fit di Stephen Hill (BakingLab) di RRT + ODT sRGB, con le stesse matrici di
// ingresso e uscita. Si discosta dalla reference di qualche percento; costa molto meno.
vec3 ToneMapACESFitted(vec3 rgb) {
    const mat3 inputMat = mat3(0.59719, 0.35458, 0.04823,
        0.07600, 0.90834, 0.01566,
        0.02840, 0.13383, 0.83777);
    const mat3 outputMat = mat3(1.60475, -0.53108, -0.07367,
        -0.10208, 1.10813, -0.00605,
        -0.00327, -0.07276, 1.07602);
    vec3 v = rgb * inputMat;
    v = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
    return clamp(v * outputMat, 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------
// AgX di Troy Sobotka, nell'approssimazione di Benjamin Wrensch (polinomio di grado 6 al posto
// della LUT). E' la base dell'AgX di Blender, ma non lo riproduce al bit: Blender usa una LUT OCIO.
vec3 ToneMapAgX(vec3 rgb) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    // queste due sono gia' scritte per colonne (come nell'originale): si usano come M * v
    vec3 v = inset * max(rgb, 1e-10);
    v = (clamp(log2(v), minEv, maxEv) - minEv) / (maxEv - minEv);
    vec3 v2 = v * v;
    vec3 v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = outset * v;
    // l'uscita e' gia' codificata per un display a gamma 2.2: torna lineare per l'encoding sRGB
    return pow(clamp(v, 0.0, 1.0), vec3(2.2));
}

// ---------------------------------------------------------------------------------------------
// Khronos PBR Neutral: lascia invariati i colori fino a circa 0.8 e comprime solo le alte luci,
// desaturandole un poco. Pensato per vedere i materiali glTF con il loro colore base.
vec3 ToneMapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if(peak < startCompression)
        return clamp(color, 0.0, 1.0);
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return clamp(mix(color, vec3(newPeak), g), 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Reinhard sulla luminanza: L / (1 + L), a cromaticita' fissa. Il bianco puro non si raggiunge mai.
vec3 ToneMapReinhard(vec3 rgb) {
    float L = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    return clamp(rgb / (1.0 + L), 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Hable / Uncharted 2 filmic, con i parametri originali: bianco a 11.2 e input raddoppiato.
vec3 HableCurve(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

vec3 ToneMapHable(vec3 rgb) {
    return clamp(HableCurve(2.0 * rgb) / HableCurve(vec3(11.2)), 0.0, 1.0);
}

vec3 ToneMap(vec3 rgb, int mapper) {
    switch(mapper) {
        case TONEMAP_ACES:        return ToneMapACES(rgb);
        case TONEMAP_ACES_FITTED: return ToneMapACESFitted(rgb);
        case TONEMAP_AGX:         return ToneMapAgX(rgb);
        case TONEMAP_NEUTRAL:     return ToneMapNeutral(rgb);
        case TONEMAP_REINHARD:    return ToneMapReinhard(rgb);
        case TONEMAP_HABLE:       return ToneMapHable(rgb);
        default:                  return rgb; // None: clamp nell'encoding sRGB
    }
}

#endif // COMMON_TONEMAP_GLSL
