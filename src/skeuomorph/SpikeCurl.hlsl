// Plain Curl for the no-flash spike (#19): a page bent around a cylinder.
cbuffer CB : register(b0) {
    float2 viewSize; float2 pagePos;
    float2 pageSize; float2 foldPoint;
    float2 foldNormal; float radius; float mode;
    float4 frontImg; // canvas rect held by texFront (x, y, w, h)
    float4 backImg;  // canvas rect held by texBack
    float2 backPagePos; float backHasImage; float pad0;
};
Texture2D texFront : register(t0);
Texture2D texBack : register(t1);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float2 pagePx : TEXCOORD0;
};

// mode 0: flat copy of texFront, 1: Curl, 2: shadow, 3: blank paper
VSOut VS(float2 uv : TEXCOORD0) {
    float2 p = uv * pageSize;
    float3 P = float3(p, 0);
    float3 N = float3(0, 0, 1);
    if (mode == 1) {
        float s = dot(p - foldPoint, foldNormal);
        if (s > 0) {
            float phi = s / radius;
            float2 base = p - foldNormal * s;
            if (phi < 3.14159265) {
                P = float3(base + foldNormal * (radius * sin(phi)), radius * (1 - cos(phi)));
                N = float3(-foldNormal * sin(phi), cos(phi));
            } else {
                P = float3(base - foldNormal * (s - 3.14159265 * radius), 2 * radius);
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

float4 PS(VSOut i, bool front : SV_IsFrontFace) : SV_Target {
    if (mode == 0) {
        // pixel-exact copy, so the layer matches SumatraPDF's own pixels
        return float4(texFront.Load(int3(i.pos.xy - frontImg.xy, 0)).rgb, 1);
    }
    if (mode == 3) {
        return float4(1, 1, 1, 1);
    }
    if (mode == 2) {
        float s = dot(i.pagePx - foldPoint, foldNormal);
        float a = s > 0 ? 0.35 * exp(-s / (radius * 2 + 8)) : 0;
        return float4(0, 0, 0, a);
    }
    float3 L = normalize(float3(0.3, -0.4, 1));
    float shade = 0.75 + 0.25 * abs(dot(normalize(i.nrm), L));
    float3 col = float3(1, 1, 1);
    if (front) {
        col = texFront.Sample(samp, (pagePos + i.pagePx - frontImg.xy) / frontImg.zw).rgb;
    } else if (backHasImage > 0) {
        float2 c = backPagePos + float2(pageSize.x - i.pagePx.x, i.pagePx.y);
        col = texBack.Sample(samp, (c - backImg.xy) / backImg.zw).rgb;
    }
    return float4(col * shade, 1);
}
