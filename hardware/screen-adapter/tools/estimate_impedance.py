import numpy as np,math,time,json
from pathlib import Path
EPS0=8.8541878128e-12;C0=299792458
def capacitance(w,s,g,h,er=4.3,mask_er=3.3,mask_thickness=.02):
 dx=.01;xx=np.arange(0,4+dx/2,dx);yy=np.arange(-h,1.5+dx/2,dx);ny,nx=len(yy),len(xx)
 X=xx[None,:];Y=yy[:,None]
 eps=np.ones((ny,nx));eps[np.broadcast_to(Y<0,eps.shape)]=er;eps[np.broadcast_to((Y>=0)&(Y<=.035+mask_thickness),eps.shape)]=mask_er
 known=np.zeros((ny,nx),bool);known[[0,-1],:]=True;known[:,[0,-1]]=True
 cond=np.broadcast_to((X>=s/2)&(X<=s/2+w)&(Y>=0)&(Y<=.035),eps.shape).copy();ground=np.broadcast_to((X>=s/2+w+g)&(Y>=0)&(Y<=.035),eps.shape)
 known|=cond|ground;val=np.zeros_like(eps);val[cond]=.5
 ex=2*eps[:,:-1]*eps[:,1:]/(eps[:,:-1]+eps[:,1:]);ey=2*eps[:-1,:]*eps[1:,:]/(eps[:-1,:]+eps[1:,:]);diag=np.zeros_like(eps);diag[:,:-1]+=ex;diag[:,1:]+=ex;diag[:-1,:]+=ey;diag[1:,:]+=ey
 def raw(v):
  a=diag*v;a[:,:-1]-=ex*v[:,1:];a[:,1:]-=ex*v[:,:-1];a[:-1,:]-=ey*v[1:,:];a[1:,:]-=ey*v[:-1,:];return a
 b=-raw(val);b[known]=0
 def A(v):a=raw(v);a[known]=0;return a
 sol=np.zeros_like(eps);r=b.copy();z=r/diag;p=z.copy();rz=np.sum(r*z);b0=math.sqrt(np.sum(b*b));iters=0
 for i in range(1800):
  ap=A(p);alpha=rz/np.sum(p*ap);sol+=alpha*p;r-=alpha*ap
  if math.sqrt(np.sum(r*r))<b0*2e-6:iters=i+1;break
  z=r/diag;new=np.sum(r*z);p=z+(new/rz)*p;rz=new
 full=sol+val;charge=np.sum(raw(full)[cond])*EPS0
 return charge,iters
def estimate(wmil,smil,gmil,hmil,er=4.3,mask_thickness=.02):
 vals=[v*.0254 for v in [wmil,smil,gmil,hmil]];c,it=capacitance(*vals,er=er,mask_thickness=mask_thickness);vac,iv=capacitance(*vals,er=1,mask_er=1,mask_thickness=mask_thickness);return {'w_mil':wmil,'s_mil':smil,'g_mil':gmil,'h_mil':hmil,'er':er,'mask_thickness_mm':mask_thickness,'grid_mm':.01,'z_diff_ohm':1/(C0*math.sqrt(c*vac)),'iterations':[it,iv],'note':'二维静电有限差分名义估算，非板厂阻抗保证'}
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description='差分共面主干名义阻抗估算；需要NumPy。')
 parser.add_argument('--width',type=float,default=16)
 parser.add_argument('--spacing',type=float,default=14)
 parser.add_argument('--ground-gap',type=float,default=6)
 parser.add_argument('--core',type=float,default=59.449)
 parser.add_argument('--er',type=float,default=4.5)
 parser.add_argument('--mask-mm',type=float,default=.01)
 a=parser.parse_args()
 result=estimate(a.width,a.spacing,a.ground_gap,a.core,er=a.er,mask_thickness=a.mask_mm)
 print(json.dumps(result,ensure_ascii=False,indent=2))
