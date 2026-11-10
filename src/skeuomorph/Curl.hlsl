// Curl layer shaders. Compiled once by compile-shaders.ps1 into CurlShaders.h;
// re-run it after changing this file.
//
// Everything is in canvas pixels. A page is a grid over pagePos..pagePos+pageSize.
// mode 0: flat, pixel-exact copy of texFront (the layer matches SumatraPDF's own pixels)
// mode 1: Curl, the page wrapped around a cylinder lying on it
// mode 2: blank paper
cbuffer CB : register(b0) {
    float2 viewSize;
    float2 pagePos;
    float2 pageSize;
    float2 foldPoint; // page px, on the fold line
    float2 foldNormal; // points to the free edge, the part that lifts
    float radius;
    float mode;
    float4 frontImg; // canvas rect held by texFront (x, y, w, h); w = 0: none
    float4 backImg; // canvas rect held by texBack; w = 0: none
    float mirrorX2; // twice the spine's canvas x: the back lands mirrored around it
    float backY; // canvas y of the landing page's top
    float backScaleY; // landing page height / turning page height
    float alpha;
    float4 paper; // blank paper, theme's page colour
};
Texture2D texFront : register(t0);
Texture2D texBack : register(t1);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float2 pagePx : TEXCOORD0;
};

static const float kPi = 3.14159265;

VSOut VS(float2 uv : TEXCOORD0) {
    float2 p = uv * pageSize;
    float3 P = float3(p, 0);
    float3 N = float3(0, 0, 1);
    if (mode == 1) {
        float s = dot(p - foldPoint, foldNormal);
        if (s > 0) {
            float phi = s / radius;
            float2 base = p - foldNormal * s;
            if (phi < kPi) {
                P = float3(base + foldNormal * (radius * sin(phi)), radius * (1 - cos(phi)));
                N = float3(-foldNormal * sin(phi), cos(phi));
            } else {
                // past half a turn: flat again, upside down on top
                P = float3(base - foldNormal * (s - kPi * radius), 2 * radius);
                N = float3(0, 0, -1);
            }
        }
    }
    float2 w = pagePos + P.xy;
    VSOut o;
    o.pos = float4(w.x / viewSize.x * 2 - 1, 1 - w.y / viewSize.y * 2, saturate(0.5 - P.z * 0.0005), 1);
    o.nrm = N;
    o.pagePx = p;
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
    return tex.Sample(samp, (c - img.xy) / img.zw).rgb;
}

float4 PS(VSOut i, bool front : SV_IsFrontFace) : SV_Target {
    if (mode == 0) {
        float2 c = i.pos.xy;
        if (!Inside(c, frontImg)) {
            return float4(paper.rgb, 1);
        }
        return float4(texFront.Load(int3(c - frontImg.xy, 0)).rgb, 1);
    }
    if (mode == 2) {
        return float4(paper.rgb * alpha, alpha);
    }

    float3 col;
    if (front) {
        col = PictureAt(texFront, frontImg, pagePos + i.pagePx);
    } else {
        float2 c = float2(mirrorX2 - (pagePos.x + i.pagePx.x), backY + i.pagePx.y * backScaleY);
        col = PictureAt(texBack, backImg, c);
    }
    // flat paper keeps its colour; the bend darkens a little
    float shade = 1 - 0.25 * (1 - abs(normalize(i.nrm).z));
    return float4(col * shade * alpha, alpha);
}
