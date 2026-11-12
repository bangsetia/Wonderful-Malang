cbuffer LightBuffer : register(b1)
{
    float3 LightDirection;
    float AmbientStrength;
    float4 LightColor;
};

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float3 Normal : TEXCOORD0;
    float2 UV : TEXCOORD1;
};

Texture2D RoadTexture : register(t0);
SamplerState RoadSampler : register(s0);

float4 main(PS_INPUT input) : SV_TARGET
{
    const float3 normal = normalize(input.Normal);
    const float3 incomingLight = normalize(-LightDirection);
    const float lightIntensity = saturate(dot(normal, incomingLight));
    const float3 lighting = AmbientStrength.xxx + LightColor.rgb * lightIntensity;
    const float4 albedo = RoadTexture.Sample(RoadSampler, input.UV);
    return float4(albedo.rgb * lighting, albedo.a);
}
