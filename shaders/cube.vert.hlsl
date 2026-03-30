cbuffer CameraData : register(b0)
{
    float4x4 u_mvp;
};

struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(u_mvp, float4(input.position, 1.0f));
    output.color = input.color;
    return output;
}
