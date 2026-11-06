// Standalone physical reference; no preview ambient floor or local light gain.
// World units and estimator match OpenVdbTransportReference.h. Every failure is
// written per path and makes the host discard the entire operation.
#include "openvdb_sparse.slang"

struct Controls {
    uint4 counts; // media, lights, triangles, spp
    uint4 limits; // rays, order, roulette start, work/path
    float4 minimumMajorant;
    float4 maximum;
    float4 environment;
    uint4 random; // seed low/high
};
struct Medium {
    float4 minimumExtinction;
    float4 maximumG;
    float4 albedo;
    float4 emission;
    uint4 dimensionsFlags;
    uint4 offsets;
    float4 nativeControls;
};
struct Light { float4 vectorType; float4 value; };
struct Triangle { float4 a; float4 b; float4 c; };
struct Ray { float4 origin; float4 direction; };
struct PathResult {
    float4 radianceCosine;
    uint4 counters; // work, scattering, nulls, shadow proposals
    uint4 diagnostics; // bit0 truncated + shadow RR terminations<<1; continuation RR; phase; errors
};
[[vk::binding(0,0)]] ConstantBuffer<Controls> controls;
[[vk::binding(1,0)]] StructuredBuffer<Medium> media;
[[vk::binding(2,0)]] StructuredBuffer<uint> payload;
[[vk::binding(3,0)]] StructuredBuffer<Light> lights;
[[vk::binding(4,0)]] StructuredBuffer<Triangle> triangles;
[[vk::binding(5,0)]] StructuredBuffer<Ray> rays;
[[vk::binding(6,0)]] RWStructuredBuffer<PathResult> results;

static const float PI = 3.14159265358979323846;
static const float FAR = 3.402823466e38;
static const uint WORK_ERROR = 1u;
static const uint NUMERIC_ERROR = 2u;
static const uint MAJORANT_ERROR = 4u;

struct State {
    uint random;
    PathResult output;
};
bool Work(inout State s) {
    if (s.output.diagnostics.w != 0u) return false;
    if (s.output.counters.x >= controls.limits.w) {
        s.output.diagnostics.w |= WORK_ERROR;
        return false;
    }
    ++s.output.counters.x;
    return true;
}
uint Hash(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}
uint RandomBits(inout State s) {
    s.random += 0x9e3779b9u;
    return Hash(s.random);
}
float Uniform(inout State s) {
    // 23 bits plus half a bin: strictly within (0,1) in Float32.
    return (float(RandomBits(s) >> 9u) + 0.5) * (1.0 / 8388608.0);
}
float Peak(float3 a) { return max(a.x,max(a.y,a.z)); }
float3 Product(float3 a, float3 b, inout State s) {
    float3 p = a*b;
    if (!all(isfinite(p)) || any((a != 0.0) & (b != 0.0) & (p == 0.0)))
        s.output.diagnostics.w |= NUMERIC_ERROR;
    return p;
}
float Product1(float a,float b,inout State s) {
    return Product(a.xxx,b.xxx,s).x;
}
void Add(inout float3 value,float3 contribution,inout State s) {
    value += contribution;
    if (!all(isfinite(value)) || any(value < 0.0)) s.output.diagnostics.w |= NUMERIC_ERROR;
}
bool Box(float3 origin,float3 direction,float limit,out float enter,out float leave) {
    enter=0.0; leave=limit;
    if (controls.minimumMajorant.w == 0.0) return false;
    [unroll] for(uint k=0u;k<3u;++k) {
        if (direction[k] == 0.0) {
            if (origin[k]<controls.minimumMajorant[k] || origin[k]>controls.maximum[k]) return false;
        } else {
            float a=(controls.minimumMajorant[k]-origin[k])/direction[k];
            float b=(controls.maximum[k]-origin[k])/direction[k];
            enter=max(enter,min(a,b)); leave=min(leave,max(a,b));
            if (leave<=enter) return false;
        }
    }
    return leave>enter;
}
float Geometry(float3 origin,float3 direction,float limit,inout State s) {
    float closest=limit;
    for(uint i=0u;i<controls.counts.z;++i) {
        if(!Work(s)) break;
        Triangle tri=triangles[i];
        float3 e1=tri.b.xyz-tri.a.xyz,e2=tri.c.xyz-tri.a.xyz;
        float3 p=cross(direction,e2);
        float determinant=dot(e1,p);
        if(determinant==0.0) continue;
        float inverse=1.0/determinant;
        if(!isfinite(inverse)) { s.output.diagnostics.w|=NUMERIC_ERROR; break; }
        float3 delta=origin-tri.a.xyz;
        float u=dot(delta,p)*inverse;
        if(u<0.0 || u>1.0) continue;
        float3 q=cross(delta,e1);
        float v=dot(direction,q)*inverse;
        if(v<0.0 || u+v>1.0) continue;
        float t=dot(e2,q)*inverse;
        if(!isfinite(t)) { s.output.diagnostics.w|=NUMERIC_ERROR; break; }
        if(t>=0.0 && t<closest) closest=t;
    }
    return closest;
}
float Density(uint id,float3 p,inout State s) {
    if(!Work(s)) return 0.0;
    Medium m=media[id];
    if(any(p<m.minimumExtinction.xyz) || any(p>m.maximumG.xyz)) return 0.0;
    if(m.dimensionsFlags.w != 0u)
        return SampleNanoVdb(payload,p,m.nativeControls.x,m.offsets.x);
    uint3 dimensions=m.dimensionsFlags.xyz;
    float3 index=clamp((p-m.minimumExtinction.xyz)/(m.maximumG.xyz-m.minimumExtinction.xyz)*
                       float3(dimensions)-0.5.xxx,0.0.xxx,float3(dimensions-1u));
    uint3 lo=uint3(index),hi=min(lo+1u,dimensions-1u);
    float3 f=index-float3(lo);
    float value=0.0;
    [unroll] for(uint corner=0u;corner<8u;++corner) {
        uint3 bit=uint3(corner&1u,(corner>>1u)&1u,(corner>>2u)&1u);
        uint3 c=uint3(bit.x!=0u?hi.x:lo.x,bit.y!=0u?hi.y:lo.y,bit.z!=0u?hi.z:lo.z);
        float3 w=lerp(1.0.xxx-f,f,float3(bit));
        value+=asfloat(payload[m.offsets.x+(c.z*dimensions.y+c.y)*dimensions.x+c.x])*w.x*w.y*w.z;
    }
    return value;
}
struct Coefficients { float extinction; float4 perMedium; float3 emission; };
Coefficients CoefficientsAt(float3 p,inout State s) {
    Coefficients c=(Coefficients)0;
    for(uint i=0u;i<controls.counts.x;++i) {
        float d=Density(i,p,s);
        c.perMedium[i]=Product1(d,media[i].minimumExtinction.w,s);
        c.extinction+=c.perMedium[i];
        c.emission+=Product(d.xxx,media[i].emission.xyz,s);
    }
    if(!isfinite(c.extinction) || !all(isfinite(c.emission))) s.output.diagnostics.w|=NUMERIC_ERROR;
    if(c.extinction>controls.minimumMajorant.w*(1.0+2e-6)) s.output.diagnostics.w|=MAJORANT_ERROR;
    return c;
}
bool Proposal(inout float t,inout float correction,float end,inout State s) {
    if(!Work(s)) return false;
    float optical=-log(1.0-Uniform(s));
    float remaining=((end-t)+correction)*controls.minimumMajorant.w;
    if(optical>=remaining) return false;
    float delta=optical/controls.minimumMajorant.w;
    // Tiny, valid free flights can round to the SAME Float32 world position.
    // Retain their distance in a compensated pair instead of rejecting an
    // otherwise well-scaled large Monte Carlo batch, or rounding flights up.
    // The high component remains the nearest representable sample position.
    precise float adjusted=delta-correction,next=t+adjusted;
    precise float nextCorrection=(next-t)-adjusted;
    if(!(delta>0.0) || !isfinite(next) || !isfinite(nextCorrection) ||
       (next==t && nextCorrection==correction)) {
        s.output.diagnostics.w|=NUMERIC_ERROR; return false;
    }
    t=next; correction=nextCorrection; return true;
}
float Transmittance(float3 p,float3 direction,float distance,inout State s) {
    if(Geometry(p,direction,distance,s)<distance) return 0.0;
    float enter,leave;
    if(!Box(p,direction,distance,enter,leave)) return 1.0;
    float t=enter,correction=0.0,result=1.0;
    while(Proposal(t,correction,leave,s)) {
        ++s.output.counters.w;
        Coefficients c=CoefficientsAt(p+direction*t,s);
        float factor=max(0.0,1.0-c.extinction/controls.minimumMajorant.w);
        result=Product1(result,factor,s);
        if(result==0.0 || s.output.diagnostics.w!=0u) break;
        if(result<1e-4) {
            float probability=result/1e-4;
            // Using the full uint32 coin avoids the open Float32 uniform's
            // 23-bit minimum bin. Reweight by the actual quantized probability,
            // not its unquantized target. Ratio factors are either exactly0
            // or >=2^-24, so with retained weight>=1e-4 this cutoff is >=256.
            uint cutoff=uint(probability*4294967296.0);
            if(cutoff==0u) { s.output.diagnostics.w|=NUMERIC_ERROR; break; }
            if(RandomBits(s)>=cutoff) {
                s.output.diagnostics.x+=2u;
                return 0.0;
            }
            result/=float(cutoff)*(1.0/4294967296.0);
        }
    }
    return result;
}
float Phase(float cosine,float g) {
    float d=1.0+g*g-2.0*g*clamp(cosine,-1.0,1.0);
    return (1.0-g*g)/(4.0*PI*d*sqrt(d));
}
float3 Direct(float3 p,float3 direction,float g,inout State s) {
    float3 value=0.0.xxx;
    for(uint i=0u;i<controls.counts.y;++i) {
        if(!Work(s)) break;
        Light light=lights[i];
        float3 toward=light.vectorType.xyz;
        float distance=FAR,falloff=1.0;
        if(light.vectorType.w!=0.0) {
            toward-=p;
            distance=length(toward);
            if(!(distance>0.0) || !isfinite(distance)) { s.output.diagnostics.w|=NUMERIC_ERROR; break; }
            toward/=distance;
            falloff=1.0/(distance*distance);
            if(!isfinite(falloff) || falloff==0.0) { s.output.diagnostics.w|=NUMERIC_ERROR; break; }
        }
        float tr=Transmittance(p,toward,distance,s);
        if(tr==0.0) continue;
        float weight=Product1(Product1(falloff,Phase(dot(direction,toward),g),s),tr,s);
        Add(value,Product(light.value.xyz,weight.xxx,s),s);
    }
    return value;
}
float3 SamplePhase(float3 current,float g,inout State s) {
    float t=2.0*Uniform(s)-1.0,d=1.0+g*t;
    float cosine=clamp((t+0.5*g*(t*t+3.0)+g*g*t+0.5*g*g*g*(t*t-1.0))/(d*d),-1.0,1.0);
    float sine=sqrt(max(0.0,1.0-cosine*cosine)),phi=2.0*PI*Uniform(s);
    float3 tangent=normalize(cross(abs(current.z)<0.9?float3(0,0,1):float3(0,1,0),current));
    return normalize(current*cosine+sine*(tangent*cos(phi)+cross(current,tangent)*sin(phi)));
}
float3 Trace(Ray ray,inout State s) {
    float3 value=0.0.xxx,beta=1.0.xxx;
    uint order=0u;
    while(s.output.diagnostics.w==0u) {
        float hit=Geometry(ray.origin.xyz,ray.direction.xyz,FAR,s),enter,leave;
        if(!Box(ray.origin.xyz,ray.direction.xyz,hit,enter,leave)) {
            if(hit==FAR) Add(value,Product(beta,controls.environment.xyz,s),s);
            return value;
        }
        bool scattered=false;
        float t=enter,correction=0.0;
        while(Proposal(t,correction,leave,s)) {
            float3 p=ray.origin.xyz+ray.direction.xyz*t;
            Coefficients c=CoefficientsAt(p,s);
            float3 source=c.emission/controls.minimumMajorant.w;
            if(any((c.emission>0.0)&(source==0.0)) || !all(isfinite(source))) s.output.diagnostics.w|=NUMERIC_ERROR;
            Add(value,Product(beta,source,s),s);
            if(s.output.diagnostics.w!=0u) return value;
            if(Uniform(s)*controls.minimumMajorant.w>=c.extinction) { ++s.output.counters.z; continue; }
            float target=Uniform(s)*c.extinction;
            uint selected=controls.counts.x-1u;
            for(uint i=0u;i<controls.counts.x;++i) {
                target-=c.perMedium[i]; if(target<0.0) { selected=i; break; }
            }
            beta=Product(beta,media[selected].albedo.xyz,s);
            if(Peak(beta)==0.0) return value;
            if(order==controls.limits.y) { s.output.diagnostics.x|=1u; return value; }
            ++order; ++s.output.counters.y;
            Add(value,Product(beta,Direct(p,ray.direction.xyz,media[selected].maximumG.w,s),s),s);
            if(s.output.diagnostics.w!=0u) return value;
            if(order>=controls.limits.z) {
                float survival=clamp(Peak(beta),0.05,0.95);
                if(Uniform(s)>=survival) { ++s.output.diagnostics.y; return value; }
                beta/=survival;
            }
            float3 next=SamplePhase(ray.direction.xyz,media[selected].maximumG.w,s);
            if(!all(isfinite(next))) { s.output.diagnostics.w|=NUMERIC_ERROR; return value; }
            ++s.output.diagnostics.z;
            s.output.radianceCosine.w+=dot(ray.direction.xyz,next);
            ray.origin.xyz=p; ray.direction.xyz=next;
            scattered=true; break;
        }
        if(!scattered) {
            if(hit==FAR) Add(value,Product(beta,controls.environment.xyz,s),s);
            return value;
        }
    }
    return value;
}
[shader("compute")]
[numthreads(64,1,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
    uint total=controls.counts.w*controls.limits.x;
    if(id.x>=total) return;
    State s=(State)0;
    s.random=Hash(controls.random.x ^ Hash(controls.random.y) ^ Hash(id.x+0x632be59bu));
    if(Work(s)) s.output.radianceCosine.xyz=Trace(rays[id.x/controls.counts.w],s);
    results[id.x]=s.output;
}
