#include "PCH.h"
#include "Camera/CinematicViewMath.h"
#include <iostream>
#include <stdexcept>

using namespace DietDrCamera;
using namespace CinematicViewMath;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static RE::NiPoint3 Column(const RE::NiMatrix3& r, int col) { return {r.entry[0][col],r.entry[1][col],r.entry[2][col]}; }
static RE::NiMatrix3 Aim(RE::NiPoint3 direction)
{
    const auto forward = direction / direction.Length();
    const float yaw = std::atan2(direction.x,direction.y);
    const RE::NiPoint3 right{std::cos(yaw),-std::sin(yaw),0};
    const auto up = right.Cross(forward);
    RE::NiMatrix3 result;
    result.entry[0][0]=forward.x; result.entry[1][0]=forward.y; result.entry[2][0]=forward.z;
    result.entry[0][1]=up.x; result.entry[1][1]=up.y; result.entry[2][1]=up.z;
    result.entry[0][2]=right.x; result.entry[1][2]=right.y; result.entry[2][2]=right.z;
    return result;
}
int main() try
{
    RE::NiFrustum base{-3.5010377f,3.5010377f,1.9693337f,-1.9693337f,5,100000,false};
    RE::NiTransform pose;
    pose.rotate = Aim({0,1,0}); pose.translate = {20,30,40}; pose.scale = 1;
    float screenX = 0, screenY = 0;
    Require(Project(pose,base,{20,1030,40},screenX,screenY) && std::abs(screenX-.5f) < .0001f && std::abs(screenY-.5f) < .0001f,
        "Subject marker misses the aim point");
    Require(Project(pose,base,{220,1030,140},screenX,screenY) && screenX > .5f && screenY < .5f,
        "Subject marker mirrors the scene horizontally or vertically");
    Require(!Project(pose,base,{20,-1030,40},screenX,screenY),"Subject behind camera drew an on-screen marker");
    for (const RE::NiPoint3 direction : {RE::NiPoint3{1,0,0},{-1,0,0},{0,-1,0},{3,4,2},{-4,2,-3}}) {
        pose.rotate = Aim(direction);
        Require(Project(pose,base,pose.translate+direction*100,screenX,screenY) &&
            std::abs(screenX-.5f) < .0001f && std::abs(screenY-.5f) < .0001f,
            "Subject marker loses alignment as native camera rotates");
    }
    auto narrow = base;
    narrow.fLeft *= .5f; narrow.fRight *= .5f; narrow.fTop *= .5f; narrow.fBottom *= .5f;
    pose.rotate = Aim({0,1,0});
    Require(!Project(pose,narrow,{520,1030,40},screenX,screenY) && Project(pose,base,{520,1030,40},screenX,screenY),
        "Subject picker ignores the engine's current FOV");
    std::cout << "Cinematic subject projection checks passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
