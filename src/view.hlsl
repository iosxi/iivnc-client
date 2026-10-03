// view.hlsl - 相手の画面を窓に描く(build.bat が fxc でバイト列にして埋め込む)
//
// 頂点は持たない。SV_VertexID から画面全体を覆う三角形を 1 つ作り、
// ビューポート(= 絵を置く矩形)いっぱいに貼る。縮めるときはミップマップを
// 使う(4K を半分にしても文字がちらつかない)。

Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VO {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VO vs(uint id : SV_VertexID)
{
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = uv;
    return o;
}

float4 ps(VO i) : SV_Target
{
    return float4(tex.Sample(smp, i.uv).rgb, 1);
}
