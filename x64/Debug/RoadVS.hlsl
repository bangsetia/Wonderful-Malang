cbuffer CameraBuffer : register(b0)
{
    row_major float4x4 View;
    row_major float4x4 Projection;
};

struct VS_INPUT
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float3 Normal : TEXCOORD0;
    float2 UV : TEXCOORD1;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    const float4 worldPosition = float4(input.Position, 1.0f);
    output.Position = mul(mul(worldPosition, View), Projection);
    output.Normal = normalize(input.Normal);
    output.UV = input.UV;
    return output;
}
