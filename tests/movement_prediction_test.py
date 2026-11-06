#!/usr/bin/env python3
"""Execute production CKZ/KZT methods with host doubles for engine/schema calls.

Regressions: full scheduling slots dropped the current split, prediction only
covered four commands, teleport angles were interpolated, and jump handling
changed the server latch independently of client prediction. No native SDK build.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, cls, name):
    marker = f"{cls}::{name}("
    if marker not in source:
        return ""
    start = source.rfind("\n", 0, source.index(marker)) + 1
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
using f32=float; using f64=double; using u32=uint32_t; using i32=int32_t;
#define MAX std::max
#define FORCED_SUBTICK_SLOTS 4
#define FRICTION_SPLIT_MARGIN 2.5f
constexpr float ENGINE_FIXED_TICK_INTERVAL=0.015625f, ENGINE_FIXED_TICK_RATE=64.0f;
constexpr int FL_ONGROUND=1, MOVETYPE_WALK=2;
constexpr uint64_t IN_JUMP=2, IN_ATTACK=1, IN_ATTACK2=2048, IN_RELOAD=8192;
constexpr int YAW=1;
struct Vector {
 float x=0,y=0,z=0;
 Vector()=default; Vector(float a,float b,float c):x(a),y(b),z(c){}
 float &operator[](int i){return (&x)[i];}
 Vector operator+(Vector b)const{return {x+b.x,y+b.y,z+b.z};}
 Vector operator-(Vector b)const{return {x-b.x,y-b.y,z-b.z};}
 Vector &operator+=(Vector b){*this=*this+b;return *this;}
 float Length2D()const{return std::sqrt(x*x+y*y);}
};
using QAngle=Vector;
template<class T>struct Field {T value{}; T& operator()(){return value;} void operator()(T v){value=v;}};
struct GameTime_t {float value=0; GameTime_t()=default; explicit GameTime_t(float f):value(f){} float GetTime(){return value;}};
struct Legacy {bool m_bOldJumpPressed=true;};
struct CCSPlayer_MovementServices {
 float m_arrForceSubtickMoveWhen[4]{};
 Field<bool> m_bUseFrictionStashedSpeed;
 Field<float> m_flUseFrictionStashedSpeedUntilFrac,m_flFrictionStashedSpeed;
 Field<GameTime_t> m_fStashGrenadeParameterWhen;
 Field<Legacy> m_LegacyJump;
 std::vector<std::pair<int,bool>> writes;
 void SetForcedSubtickMove(int i,float t,bool network=true){m_arrForceSubtickMoveWhen[i]=t;writes.push_back({i,network});}
};
struct ViewAngleServerChange_t {Field<u32> nIndex;};
template<class T>struct CSchemaCollection {
 std::vector<T*> items; int Count(){return items.size();} T* Element(int i){return items[i];}
};
struct CCSPlayerPawn {
 Field<u32> m_fFlags; Field<float> m_flActualGravityScale,m_flVelocityModifier;
 CSchemaCollection<ViewAngleServerChange_t> changes;
 CSchemaCollection<ViewAngleServerChange_t> m_ServerViewAngleChanges(){return changes;}
};
struct bbox_t {Vector mins{-16,-16,0},maxs{16,16,72};};
struct trace_t {bool m_bStartInSolid=false;float m_flFraction=1;};
struct CTraceFilterPlayerMovementCS {explicit CTraceFilterPlayerMovementCS(CCSPlayerPawn*){}};
struct Ray_t {Vector mins,maxs;Ray_t(Vector a,Vector b):mins(a),maxs(b){}};
struct INavPhysicsInterface {
 static inline trace_t next{};static inline Vector lastEnd;static inline Ray_t lastBounds{{},{}};static inline int calls=0;
 static void TraceShape(Ray_t r,Vector,Vector end,CTraceFilterPlayerMovementCS*,trace_t*out){*out=next;lastEnd=end;lastBounds=r;calls++;}
};
struct MoveData {QAngle m_vecViewAngles;Vector m_vecAbsOrigin,m_vecVelocity;};
struct Player {
 CCSPlayer_MovementServices services;CCSPlayerPawn pawn;bool valid=true,processingMovement=false;int moveType=MOVETYPE_WALK;
 Vector origin{0,0,100},velocity{300,0,200},baseVelocity;MoveData data,moveDataPost;
 MoveData*currentMoveData=&data;
 CCSPlayer_MovementServices*GetMoveServices(){return valid?&services:nullptr;}
 CCSPlayerPawn*GetPlayerPawn(){return valid?&pawn:nullptr;}
 int GetMoveType(){return moveType;}
 void GetOrigin(Vector*v){*v=origin;}void GetVelocity(Vector*v){*v=velocity;}void GetBaseVelocity(Vector*v){*v=baseVelocity;}
 void GetBBoxBounds(bbox_t*b){*b=bbox_t{};}
};
struct Globals {int tickcount=100;float curtime=100.5f/64,frametime=1.0f/128;};
using CGlobalVars=Globals;
struct Utils {Globals globals;Globals*GetGlobals(){return &globals;}Globals*GetServerGlobals(){return &globals;}};
Utils utils;Utils*g_pKZUtils=&utils;
struct Cvar {float GetFloat(){return 800;}};Cvar gravity;
constexpr int MODECVAR_SV_GRAVITY=0;
namespace KZ::mode {Cvar*modeCvarRefs[]={&gravity};}
struct Step {uint64_t key;bool down;float time;uint64_t button(){return key;}bool pressed(){return down;}float when(){return time;}void set_when(float v){time=v;}};
using CSubtickMoveStep=Step;
struct BaseCmd {std::vector<Step> moves;u32 consumed=0;int subtick_moves_size(){return moves.size();}Step*mutable_subtick_moves(int i){return &moves[i];}u32 consumed_server_angle_changes()const{return consumed;}};
struct PlayerCommand {BaseCmd cmd;BaseCmd*mutable_base(){return &cmd;}const BaseCmd&base(){return cmd;}};
struct Service {
 Player *player;bool hasValidDesiredViewAngle=true,serverAngleChange=false;u32 lastConsumedAngleChange=0;
 bool touchedSlopeLastMove=false;float lastTakeoffZ=0,landHeight=0,lastLandingSpeed=0,lastLandingSpeedTime=0;
 QAngle lastValidDesiredViewAngle;float lastJumpReleaseTime=0;float jumpPressTimes[4]={-1,-1,-1,-1};int jumpPressIdx=0;
 explicit Service(Player*p):player(p){}
 void OnPhysicsSimulate();void OnPhysicsSimulatePost();void OnSetupMove(PlayerCommand*);
 void OnTeleport(const Vector*,const QAngle*,const Vector*);void ResetPrestrafe(){}
 void InterpolateViewAngles();bool CanUseFrictionSplit();void ClearFrictionSplit();void Cleanup();
};
int failures=0;
void check(bool ok,const char*name){if(!ok){std::printf("FAIL: %s\n",name);failures++;}}
bool close(float a,float b){return std::fabs(a-b)<0.000001f;}
'''

CASES = r'''
int main(){
 {Player p;Service s(&p);float initial[]={2,3,4,5};std::copy(initial,initial+4,p.services.m_arrForceSubtickMoveWhen);
  s.OnPhysicsSimulate();check(close(p.services.m_arrForceSubtickMoveWhen[3],1.5546875f),"full future slots: current split replaces furthest");
  auto n=p.services.writes.size();s.OnPhysicsSimulate();check(p.services.writes.size()==n,"existing current split is not duplicated");}
 {Player p;Service s(&p);p.pawn.m_flActualGravityScale(1);INavPhysicsInterface::next={false,1};
  s.OnPhysicsSimulatePost();
  check(p.services.m_bUseFrictionStashedSpeed(),"safe air enables next-command prediction split");
  check(close(p.services.m_flFrictionStashedSpeed(),300),"stash preserves horizontal speed");
  check(close(p.services.m_arrForceSubtickMoveWhen[0],1.5859375f),"four slots start after friction split");
  check(close(p.services.m_fStashGrenadeParameterWhen().GetTime(),1.6484375f),"sixth predicted command covered");
  check(close(INavPhysicsInterface::lastEnd.z,97.40234375f),"gravity sweep covers next tick plus snap margin");
  check(close(INavPhysicsInterface::lastBounds.mins.x,-23.1875f),"trace includes horizontal reach");
  p.pawn.m_fFlags(FL_ONGROUND);INavPhysicsInterface::calls=0;s.OnPhysicsSimulatePost();
  check(INavPhysicsInterface::calls==0,"ground does not run airborne sweep");}
 {Player p;Service s(&p);p.pawn.m_flActualGravityScale(1);p.services.m_bUseFrictionStashedSpeed(true);
  INavPhysicsInterface::next={false,.8f};s.OnPhysicsSimulatePost();
  check(!p.services.m_bUseFrictionStashedSpeed(),"possible ground contact clears airborne stash");
  check(close(p.services.m_arrForceSubtickMoveWhen[0],1.5703125f),"ground risk uses ordinary first split");}
 {Player p;Service s(&p);p.pawn.m_flActualGravityScale(1);INavPhysicsInterface::next={true,1};s.OnPhysicsSimulatePost();
  check(!p.services.m_bUseFrictionStashedSpeed(),"startsolid cannot use friction stash");
  p.moveType=9;INavPhysicsInterface::calls=0;s.OnPhysicsSimulatePost();check(INavPhysicsInterface::calls==0,"noclip skips air sweep");}
 {Player p;Service s(&p);p.services.m_bUseFrictionStashedSpeed(true);p.services.m_fStashGrenadeParameterWhen(GameTime_t(9));s.Cleanup();
  check(!p.services.m_bUseFrictionStashedSpeed(),"cleanup clears injected air stash");
  check(close(p.services.m_fStashGrenadeParameterWhen().GetTime(),0),"cleanup clears grenade prediction time");}
 {Player p;Service s(&p);Vector destination{1,2,3};p.services.m_bUseFrictionStashedSpeed(true);
  s.OnTeleport(&destination,nullptr,nullptr);check(!p.services.m_bUseFrictionStashedSpeed(),"teleport clears air prediction stash");}
 {Player p;Service s(&p);PlayerCommand cmd;cmd.cmd.consumed=5;p.data.m_vecViewAngles.y=90;
  s.OnSetupMove(&cmd);s.InterpolateViewAngles();check(close(p.data.m_vecViewAngles.y,90),"newly consumed teleport angle is not interpolated");
  p.data.m_vecViewAngles.y=90;s.OnSetupMove(&cmd);s.InterpolateViewAngles();check(close(p.data.m_vecViewAngles.y,45),"normal turning still interpolates");
  ViewAngleServerChange_t pending;pending.nIndex(6);p.pawn.changes.items={&pending};p.data.m_vecViewAngles.y=90;
  s.OnSetupMove(&cmd);s.InterpolateViewAngles();check(close(p.data.m_vecViewAngles.y,90),"unacknowledged server angle is not interpolated");}
 {Player p;Service s(&p);PlayerCommand cmd;cmd.cmd.moves={{IN_JUMP,true,.25f},{IN_JUMP,false,.75f},{IN_ATTACK,true,.31f}};
  s.OnSetupMove(&cmd);check(p.services.m_LegacyJump().m_bOldJumpPressed,"jump press does not reset server-only legacy latch");
  check(close(cmd.cmd.moves[2].time,.31f),"weapon input is untouched");
  if(KZT){check(close(s.jumpPressTimes[0],1.55078125f),"KZT retains real jump input timestamp");check(close(cmd.cmd.moves[0].time,.25f),"KZT retains real subtick input");}
  else{check(close(cmd.cmd.moves[0].time,0)&&close(cmd.cmd.moves[1].time,.5f),"CKZ retains half-tick input quantization");}}
 std::printf("%s: %s (%d failures)\n",KZT?"KZT":"CKZ",failures?"FAIL":"PASS",failures);return failures?1:0;
}
'''


def main():
    failed = False
    for mode, cls in (("ckz", "KZClassicModeService"), ("kzt", "KZTimerModeService")):
        path = f"src/kz/mode/kz_mode_{mode}.cpp"
        source = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{os.environ['KZ_TEST_REF']}:{path}"], text=True) if os.environ.get("KZ_TEST_REF") else (ROOT / path).read_text()
        names = ("OnPhysicsSimulate", "OnPhysicsSimulatePost", "CanUseFrictionSplit", "ClearFrictionSplit", "OnSetupMove", "InterpolateViewAngles", "Cleanup", "OnTeleport")
        bodies = "\n".join(method(source, cls, n).replace(cls + "::", "Service::") for n in names)
        code = STUBS + bodies + f"\nconstexpr bool KZT={'true' if mode == 'kzt' else 'false'};\n" + CASES
        with tempfile.TemporaryDirectory(prefix="kz-prediction-") as folder:
            cpp = Path(folder) / "test.cpp"
            cpp.write_text(code)
            binary = Path(folder) / "test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter", str(cpp), "-o", str(binary)], check=True)
            failed |= subprocess.run([str(binary)], check=False).returncode != 0
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
