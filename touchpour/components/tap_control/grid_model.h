#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace tap {
constexpr const char *vessel_names[]={"Small cup","Large cup","Pitcher"};
struct GridFrame {
  int16_t mm[64]{};
  uint8_t status[64]{};
  uint32_t at=0, sequence=0;
  bool connected=false;
  bool valid(int i)const{return connected && (status[i]==5 || status[i]==9) && mm[i]>=20 && mm[i]<=2000;}
};
inline int bits(uint64_t v){return __builtin_popcountll(v);}
inline int grid_median(int *v,int n){std::sort(v,v+n);return v[n/2];}
inline uint64_t shift_mask(uint64_t mask,int dx,int dy){
  uint64_t out=0;
  for(int i=0;i<64;++i)if(mask&(1ULL<<i)){
    int x=i%8+dx,y=i/8+dy;if(x>=0&&x<8&&y>=0&&y<8)out|=1ULL<<(y*8+x);
  }return out;
}
inline uint64_t interior(uint64_t m){
  uint64_t out=0;
  for(int y=1;y<7;++y)for(int x=1;x<7;++x){int i=y*8+x;
    uint64_t cross=(1ULL<<i)|(1ULL<<(i-1))|(1ULL<<(i+1))|(1ULL<<(i-8))|(1ULL<<(i+8));
    if((m&cross)==cross)out|=1ULL<<i;
  }return out;
}
inline int median_mask(const GridFrame &f,uint64_t m,bool minimum=false){
  int v[64],n=0;for(int i=0;i<64;++i)if(m&(1ULL<<i)){if(!f.valid(i))return -1;v[n++]=f.mm[i];}
  if(!n)return -1;
  return minimum?*std::min_element(v,v+n):grid_median(v,n);
}
struct GridExample {uint64_t mask=0,inside=0;int16_t rim[64]{};};
struct VesselProfile {
  GridExample examples[8]{};
  uint8_t count=0;
  bool full=false,half=false;
  int16_t full_gap=0,half_gap=0;
};
struct GridStore {
  uint32_t version=1;
  int16_t background[64]{};
  bool background_ready=false,enabled=false;
  VesselProfile profiles[3]{};
};
inline bool valid_grid_store(const GridStore &s){
  if(s.version!=1 || (s.enabled&&!s.background_ready))return false;
  if(s.background_ready)for(int d:s.background)if(d<50||d>2000)return false;
  for(const auto &p:s.profiles){
    if(p.count>8 || (p.full&&(!p.count||p.full_gap<0||p.full_gap>1000)) ||
       (p.half&&(!p.full||p.half_gap<=p.full_gap||p.half_gap>1500)))return false;
    for(int e=0;e<p.count;++e)if(bits(p.examples[e].inside)<3 ||
       (p.examples[e].inside&p.examples[e].mask)!=p.examples[e].inside)return false;
  }return true;
}
struct GridMatch {
  int profile=-1,example=-1,dx=0,dy=0,score=0,rim=-1,level=-1,nearest=-1;
  bool recognized=false;
  uint64_t mask=0,inside=0;
};
inline uint64_t foreground(const GridStore &s,const GridFrame &f){
  uint64_t mask=0;if(!s.background_ready)return 0;
  for(int i=0;i<64;++i)if(f.valid(i)&&f.mm[i]<s.background[i]-30)mask|=1ULL<<i;
  return mask;
}
// Compare occupancy and boundary depth after translation. Interior depth is
// deliberately excluded: filling a trained vessel must not change its identity.
inline GridMatch match_grid(const GridStore &s,const GridFrame &f){
  GridMatch best;int runner=0;const uint64_t observed=foreground(s,f);
  if(bits(observed)<6)return best;
  for(int p=0;p<3;++p){
    GridMatch candidate;
    for(int e=0;e<s.profiles[p].count;++e){const auto &t=s.profiles[p].examples[e];
      for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx){
        uint64_t moved=shift_mask(t.mask,dx,dy);
        if(bits(moved)!=bits(t.mask))continue; // clipped vessels cannot authorize a pour
        int score=100*bits(moved&observed)/bits(moved|observed);
        if(score<75)continue;
        uint64_t inside=shift_mask(t.inside,dx,dy),edge=moved&~inside;
        int changes[64],n=0;
        for(int i=0;i<64;++i)if((t.mask&~t.inside)&(1ULL<<i)){
          int j=i+dy*8+dx;if(!f.valid(j)){n=0;break;}changes[n++]=f.mm[j]-t.rim[i];
        }
        if(n<3)continue;
        const int offset=grid_median(changes,n);if(std::abs(offset)>80)continue;
        int deviation=0;for(int i=0;i<n;++i)deviation+=std::abs(changes[i]-offset);
        score-=std::min(30,deviation/n);
        if(score>candidate.score){
          candidate={p,e,dx,dy,score,median_mask(f,edge),median_mask(f,inside),median_mask(f,inside,true),false,moved,inside};
        }
      }
    }
    if(candidate.score>best.score){runner=best.score;best=candidate;}
    else runner=std::max(runner,candidate.score);
  }
  best.recognized=best.score>=75 && best.score-runner>=12 && best.level>0 && best.rim>0;
  return best;
}
inline bool capture_background(GridStore &s,const GridFrame &f){
  for(int i=0;i<64;++i)if(!f.valid(i)||f.mm[i]<50)return false;
  s=GridStore{};s.background_ready=true;
  for(int i=0;i<64;++i)s.background[i]=f.mm[i];
  return true;
}
inline bool capture_example(GridStore &s,const GridFrame &f,int p){
  if(p<0||p>2||!s.background_ready)return false;
  auto &v=s.profiles[p];if(v.count>=8)return false;
  uint64_t mask=foreground(s,f),inner=interior(mask);
  constexpr uint64_t border=0xff818181818181ffULL;
  if(mask&border || bits(inner)<3)return false;
  auto &t=v.examples[v.count++];t.mask=mask;t.inside=inner;
  for(int i=0;i<64;++i)t.rim[i]=f.mm[i];
  return true;
}
inline bool capture_target(GridStore &s,const GridFrame &f,int p,bool half){
  if(p<0||p>2)return false;
  // Match against this profile only: the operator explicitly selected its label.
  GridStore one=s;for(int i=0;i<3;++i)if(i!=p)one.profiles[i]=VesselProfile{};
  auto m=match_grid(one,f);if(!m.recognized || m.level<m.rim)return false;
  int gap=m.level-m.rim;auto &v=s.profiles[p];
  if(half){if(!v.full||gap<=v.full_gap)return false;v.half_gap=gap;v.half=true;}
  else {v.full_gap=gap;v.full=true;v.half=false;v.half_gap=0;}
  return true;
}
} // namespace tap
