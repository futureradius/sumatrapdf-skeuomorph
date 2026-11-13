// Curl layer shaders. Compiled once by compile-shaders.ps1 into CurlShaders.h;
// re-run it after changing this file.
//
// Canvas pixels throughout: x right, y down, z up toward the reader.
// mode 0: flat, pixel-exact copy of texFront over rect
// mode 1: the sheet, wrapped over a cylinder lying on it (corner peel, spec 3.3)
// mode 2: blank paper over rect
// mode 3: the sheet's shadow, flattened onto the paper below
//
// The sheet lies flat on the start side of the spine before the turn and on
// the other side after it, mirrored. Its outline changes from startRect to
// endRect on the way (pages of different sizes). Bend() works in "frame"
// units: x from the spine toward the start side, y up from the sheet's
// bottom edge, so the bottom outer corner lifts first. Port of
// prototypes/curl-look-prototype.html (branch prototype/curl-look).
cbuffer CB : register(b0) {
    float2 viewSize;
    float mode;
    float alpha;
    float4 paper;     // blank paper, theme's page colour
    float4 rect;      // modes 0, 2: canvas rect (x, y, w, h)
    float4 frontImg;  // canvas rect held by texFront; w = 0: none
    float4 backImg;   // canvas rect held by texBack; w = 0: none
    float4 startRect; // the sheet lying flat before the turn
    float4 endRect;   // and after it
    float spineX;
    float dirX;     // +1: the start side is right of the spine
    float progress; // p = 0.5 - 0.5 cos(pi t)
    float lead;     // fold slant, radians
    float radius;
    float camDist; // camera height over the view's centre
    float2 shadowOff;
    float shadowAlpha;
    float shading;
    float2 pad;
};
Texture2D texFront : register(t0);
Texture2D texBack : register(t1);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;       // the start face's side
    float3 world : TEXCOORD0;  // canvas px, z up
    float2 atStart : TEXCOORD1; // this point in the start face's picture
    float2 atEnd : TEXCOORD2;   // and in the end face's
    float height : TEXCOORD3;   // shadow: how high the paper casting it is
    float reach : TEXCOORD4;    // spine to the sheet's outer edge
};

static const float kPi = 3.14159265;
static const float kDepthPerPx = 0.0005;
// light from the upper left; the shadow falls away from it
static const float3 kLight = float3(-0.35, -0.45, 1);
static const float2 kShadowPush = float2(0.22, 0.28);
static const float kShadowGrow = 0.035;

// the camera looks straight down at the view's centre; paper on the page
// plane (z = 0) lands on exact pixels, lifted paper grows a little
float4 Project(float3 P) {
    float2 c = viewSize * 0.5;
    float w = (camDist - P.z) / camDist;
    float2 s = c + (P.xy - c) / w;
    float2 ndc = float2(s.x / viewSize.x * 2 - 1, 1 - s.y / viewSize.y * 2);
    return float4(ndc * w, saturate(0.5 - P.z * kDepthPerPx) * w, w);
}

struct Bent {
    float3 P;
    float3 N;
    float reach;
};

// the sheet's outline now (pages of different sizes change shape), frame units
struct Sheet {
    float gap; // spine to the sheet's spine edge
    float w;
    float h;
    float top; // canvas y
    float2 n;  // fold normal, toward the lifting corner
    float fold; // the fold line: dot(q, n) == fold
};

Sheet SheetNow() {
    float p = progress;
    float gapStart = dirX > 0 ? startRect.x - spineX : spineX - (startRect.x + startRect.z);
    float gapEnd = dirX > 0 ? spineX - (endRect.x + endRect.z) : endRect.x - spineX;
    Sheet s;
    s.gap = lerp(gapStart, gapEnd, p);
    s.w = lerp(startRect.z, endRect.z, p);
    s.h = lerp(startRect.w, endRect.w, p);
    s.top = lerp(startRect.y, endRect.y, p);
    // the fold line starts through the bottom outer corner, slanted, and
    // sweeps to the spine
    s.n = float2(cos(lead), -sin(lead));
    s.fold = (1 - p) * (s.gap + s.w) * s.n.x;
    return s;
}

// shadow: the grid spread over the lifted part only, past the fold line
// (the rest casts none; 8 copies over the whole page cost late frames at 4K)
float2 LiftedUv(Sheet s, float2 uv) {
    float u0 = (s.fold - (1 - uv.y) * s.h * s.n.y - s.gap * s.n.x) / (s.w * s.n.x);
    return float2(lerp(saturate(u0), 1, uv.x), uv.y);
}

// uv: x 0 at the sheet's spine edge .. 1 at its outer edge, y 0 top .. 1 bottom.
// Past the fold line the paper wraps over the cylinder, past half a turn it
// lies flat again, upside down on top
Bent Bend(Sheet s, float2 uv) {
    float2 q = float2(s.gap + uv.x * s.w, (1 - uv.y) * s.h);
    float2 n = s.n;
    float d = dot(q, n) - s.fold;
    float halfTurn = kPi * radius;
    float3 P = float3(q, 0);
    float3 N = float3(0, 0, 1);
    if (d > 0 && d < halfTurn) {
        float a = d / radius;
        P = float3(q + n * (radius * sin(a) - d), radius * (1 - cos(a)));
        N = float3(-n * sin(a), cos(a));
    } else if (d >= halfTurn) {
        P = float3(q - n * (2 * d - halfTurn), 2 * radius);
        N = float3(0, 0, -1);
    }

    Bent b;
    b.P = float3(spineX + dirX * P.x, s.top + s.h - P.y, P.z);
    b.N = float3(dirX * N.x, -N.y, N.z);
    b.reach = s.gap + s.w;
    return b;
}

VSOut VS(float2 uv : TEXCOORD0) {
    VSOut o;
    o.nrm = float3(0, 0, 1);
    o.atStart = 0;
    o.atEnd = 0;
    o.height = 0;
    o.reach = 1;
    if (mode == 0 || mode == 2) {
        o.world = float3(rect.xy + uv * rect.zw, 0);
        o.pos = Project(o.world);
        return o;
    }

    Sheet sheet = SheetNow();
    if (mode == 3) {
        uv = LiftedUv(sheet, uv);
    }
    Bent b = Bend(sheet, uv);
    o.reach = b.reach;
    o.height = b.P.z;
    if (mode == 3) {
        float2 xy = b.P.xy + kShadowPush * b.P.z + shadowOff * (1 + b.P.z * kShadowGrow);
        o.world = float3(xy, 0);
        o.pos = Project(o.world);
        return o;
    }

    o.world = b.P;
    o.nrm = b.N;
    o.pos = Project(b.P);
    float sx = dirX > 0 ? startRect.x + uv.x * startRect.z : startRect.x + (1 - uv.x) * startRect.z;
    float ex = dirX > 0 ? endRect.x + (1 - uv.x) * endRect.z : endRect.x + uv.x * endRect.z;
    o.atStart = float2(sx, startRect.y + uv.y * startRect.w);
    o.atEnd = float2(ex, endRect.y + uv.y * endRect.w);
    return o;
}

bool Inside(float2 c, float4 img) {
    return img.z > 0 && c.x >= img.x && c.y >= img.y && c.x < img.x + img.z && c.y < img.y + img.w;
}

// a picture of canvas point c, or blank paper where the picture has none
float3 PictureAt(Texture2D tex, float4 img, float2 c) {
    if (!Inside(c, img)) {
        return paper.rgb;
    }
    return tex.SampleLevel(samp, (c - img.xy) / img.zw, 0).rgb;
}

float4 PS(VSOut i) : SV_Target {
    if (mode == 0) {
        float2 c = i.pos.xy;
        if (!Inside(c, frontImg)) {
            return float4(paper.rgb * alpha, alpha);
        }
        return float4(texFront.Load(int3(c - frontImg.xy, 0)).rgb * alpha, alpha);
    }
    if (mode == 2) {
        return float4(paper.rgb * alpha, alpha);
    }
    if (mode == 3) {
        // soft and faint near the page, fading as the paper rises
        float a = shadowAlpha * smoothstep(1.5, i.reach * 0.08, i.height) *
                  (1 - 0.65 * smoothstep(0, i.reach * 0.9, i.height));
        // paper just past the fold casts none: skip the blend
        if (a <= 0) {
            discard;
        }
        return float4(0, 0, 0, a);
    }

    // whichever face looks at the camera; flat paper keeps its colour
    float3 n = normalize(i.nrm);
    float3 eye = float3(viewSize * 0.5, camDist);
    float3 V = normalize(eye - i.world);
    // one picture per pixel, not both
    float3 col;
    [branch] if (dot(n, V) >= 0) {
        col = PictureAt(texFront, frontImg, i.atStart);
    } else {
        col = PictureAt(texBack, backImg, i.atEnd);
        n = -n;
    }

    float3 L = normalize(kLight);
    float diffuse = max(dot(n, L), 0) / L.z;
    float spec = pow(max(dot(n, normalize(L + V)), 0), 50) * (1 - n.z);
    col *= lerp(1, 0.2 + 0.8 * diffuse, shading);
    col = saturate(col + shading * 0.35 * spec);
    return float4(col * alpha, alpha);
}
