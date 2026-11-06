#!/usr/bin/env python3
"""Compile actual pawn declarations and collection accessor against own-field schema.

The engine owns m_ServerViewAngleChanges on CBasePlayerPawn. Binding it on
CCSPlayerPawn must return an invalid collection with our current schema lookup.
Only unrelated SDK includes/scalar accessors are stubbed; the collection macro
and both pawn declarations are production source.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
schema = (ROOT / "src/utils/schema.h").read_text()
collection_macro = schema[schema.index("#define SCHEMA_FIELD_COLLECTION"):schema.index("// Use this when you want the member's value itself")]
declarations = schema[schema.index("#define DECLARE_SCHEMA_CLASS_BASE"):schema.index("// clang-format on", schema.index("#define DECLARE_SCHEMA_CLASS_BASE"))]

prefix = r'''
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
using uint32=uint32_t;using uint8=uint8_t;
constexpr uint32_t hash_32_fnv1a_const(const char*s,uint32_t v=0x811c9dc5){return *s?hash_32_fnv1a_const(s+1,(v^uint32_t(*s))*0x1000193):v;}
struct CSchemaCollectionField{};
template<class T>struct CSchemaCollection {bool valid;CSchemaCollection(CSchemaCollectionField*,void*p):valid(p){}int Count(){return valid?1:0;}};
struct SchemaKey {uint32_t offset=0;bool networked=false;};
namespace schema {
SchemaKey GetOffset(const char*,uint32_t,const char*,uint32_t){return {};}
int FindChainOffset(const char*,uint32_t){return 0;}
void*GetCollectionManipulator(const char*owner,const char*field){
 return !std::strcmp(owner,"CBasePlayerPawn")&&!std::strcmp(field,"m_ServerViewAngleChanges")?reinterpret_cast<void*>(1):nullptr;
}
}
void ChainNetworkStateChanged(uintptr_t,uint32_t){}void EntityNetworkStateChanged(uintptr_t,uint32_t){}void NetworkVarStateChanged(uintptr_t,uint32_t,int){}
template<class T>struct Scalar {T v{};T&operator()(){return v;}void operator()(T x){v=x;}};
#define SCHEMA_FIELD(type,name) Scalar<type> name;
#define CALL_VIRTUAL(...) ((void)0)
struct QAngle{};struct CEconItemView{};class CBasePlayerController;
template<class T>struct CHandle{};
constexpr uint32_t FL_BOT=16;
struct CBaseModelEntity {Scalar<bool>m_bTakesDamage;Scalar<uint32_t>m_fFlags;};
'''


def without_includes(path):
    return "\n".join(line for line in path.read_text().splitlines() if not line.startswith(("#include", "#pragma once")))


def main():
    code = prefix + collection_macro + declarations
    for name in ("cbaseplayerpawn.h", "ccsplayerpawn.h"):
        code += without_includes(ROOT / "src/sdk/entity" / name)
    code += r'''
int main(){
 alignas(CCSPlayerPawn) unsigned char storage[sizeof(CCSPlayerPawn)]{};
 auto*pawn=reinterpret_cast<CCSPlayerPawn*>(storage);
 bool ok=pawn->m_ServerViewAngleChanges().Count()==1;
 std::puts(ok?"SDK angle collection: PASS":"SDK angle collection: FAIL (pending angles hidden by wrong schema owner)");
 return ok?0:1;
}
'''
    with tempfile.TemporaryDirectory(prefix="kz-schema-") as folder:
        source = Path(folder) / "test.cpp"
        source.write_text(code)
        binary = Path(folder) / "test"
        subprocess.run(["g++", "-std=c++17", "-Wno-invalid-offsetof", str(source), "-o", str(binary)], check=True)
        raise SystemExit(subprocess.run([str(binary)]).returncode)


if __name__ == "__main__":
    main()
