#include "../components/tap_control/grid_model.h"
#include <cassert>
#include <cstdio>
using namespace tap;
GridFrame tray(){GridFrame f;f.connected=true;for(int i=0;i<64;++i){f.mm[i]=600;f.status[i]=5;}return f;}
GridFrame vessel(int width,int height,int dx=0,int dy=0,int fill=300,int lift=0){
  auto f=tray();for(int y=2+dy;y<2+dy+height;++y)for(int x=2+dx;x<2+dx+width;++x){
    if(x<0||x>7||y<0||y>7)continue;
    bool edge=y==2+dy||y==1+dy+height||x==2+dx||x==1+dx+width;
    f.mm[y*8+x]=(edge?180:fill)+lift;
  }return f;
}
int main(){
  GridStore s;assert(!s.background_ready);auto bg=tray();bg.status[4]=255;assert(!capture_background(s,bg));
  assert(capture_background(s,tray()));assert(valid_grid_store(s));
  auto a=vessel(3,5);assert(capture_example(s,a,0));
  auto b=vessel(5,3);assert(capture_example(s,b,1));
  auto c=vessel(5,5);assert(capture_example(s,c,2));
  for(int p=0;p<3;++p){const int w=p==0?3:5,h=p==1?3:5;
    assert(capture_target(s,vessel(w,h,0,0,220),p,false));
    assert(capture_target(s,vessel(w,h,0,0,260),p,true));
    assert(!capture_target(s,vessel(w,h,0,0,210),p,true));
    for(int dx=-1;dx<=0;++dx)for(int dy=-1;dy<=0;++dy){
      auto f=vessel(w,h,dx,dy,290,-25);auto m=match_grid(s,f);
      assert(m.recognized&&m.profile==p&&m.level==265&&m.rim==155);
      assert(m.rim+s.profiles[p].full_gap==195);
    }
  }
  assert(!match_grid(s,tray()).recognized);auto invalid=vessel(3,5);invalid.connected=false;assert(!match_grid(s,invalid).recognized);
  auto occluded=vessel(3,5);occluded.status[3*8+3]=255;assert(!match_grid(s,occluded).recognized);
  assert(!capture_example(s,vessel(5,5,1),0)); // edge-clipped
  assert(!capture_example(s,vessel(2,2),0)); // insufficient interior
  auto ambiguous=s;ambiguous.profiles[1]=ambiguous.profiles[0];assert(!match_grid(ambiguous,a).recognized);
  for(int i=1;i<8;++i){assert(capture_example(s,a,0));}assert(!capture_example(s,a,0));
  assert(capture_target(s,vessel(3,5,0,0,225),0,false));assert(!s.profiles[0].half);
  assert(valid_grid_store(s));s.profiles[0].count=9;assert(!valid_grid_store(s));
  assert(capture_background(s,tray()));assert(!s.profiles[0].count&&!s.enabled);
  printf("PASS: three profiles, shifted/lifted vessels, independent full/half targets, empty tray, lost/invalid interior, ambiguity, clipped/undersized rejection, eight-example bound and retraining\n");
}
