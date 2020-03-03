Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PixelShaderInput
{
    float4 Color    : COLOR;
};
 
float4 main( PixelShaderInput IN ) : SV_Target
{
    return IN.Color;
}