param([Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$Pdb,
      [Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'

if ((Get-Item -LiteralPath $Dll).VersionInfo.FileVersion -ne $Version) {
    throw 'DLL file version does not match the project.'
}
if (-not ('DdcRelease.BinaryCheck' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
namespace DdcRelease {
    public static class BinaryCheck {
        static uint U(byte[] b, int p) { return BitConverter.ToUInt32(b, p); }
        static int Rva(byte[] b, int pe, uint rva) {
            int count = BitConverter.ToUInt16(b, pe + 6);
            int sections = pe + 24 + BitConverter.ToUInt16(b, pe + 20);
            for (int i = 0; i < count; ++i) {
                int s = sections + 40 * i;
                uint start = U(b, s + 12), size = Math.Max(U(b, s + 8), U(b, s + 16));
                if (rva >= start && rva - start < size) return checked((int)(U(b, s + 20) + rva - start));
            }
            throw new Exception("PE address outside sections");
        }
        static string Z(byte[] b, int p) {
            return Encoding.ASCII.GetString(b, p, Array.IndexOf(b, (byte)0, p) - p);
        }
        static Guid GuidAt(byte[] b, int p) {
            byte[] value = new byte[16]; Array.Copy(b, p, value, 0, 16); return new Guid(value);
        }
        static int ResourceEntry(byte[] b, int root, int directory, uint? id) {
            int count = BitConverter.ToUInt16(b, directory + 12) + BitConverter.ToUInt16(b, directory + 14);
            for (int i = 0; i < count; ++i) {
                int entry = directory + 16 + i * 8;
                if (!id.HasValue || U(b, entry) == id.Value)
                    return checked(root + (int)(U(b, entry + 4) & 0x7fffffff));
            }
            throw new Exception("DLL license-notice resource is missing");
        }
        public static string ReadLicenseNotices(string dll) {
            byte[] b = File.ReadAllBytes(dll);
            int pe = checked((int)U(b, 60)), opt = pe + 24;
            int root = Rva(b, pe, U(b, opt + 128));
            int type = ResourceEntry(b, root, root, 10); // RT_RCDATA
            int name = ResourceEntry(b, root, type, 2000);
            int data = ResourceEntry(b, root, name, null); // language
            return Encoding.UTF8.GetString(b, Rva(b, pe, U(b, data)), checked((int)U(b, data + 4)));
        }
        public static string Verify(string dll, string pdb, string expectedVersion) {
            byte[] b = File.ReadAllBytes(dll);
            int pe = checked((int)U(b, 60)), opt = pe + 24;
            if (U(b, pe) != 0x4550 || BitConverter.ToUInt16(b, pe + 4) != 0x8664 ||
                BitConverter.ToUInt16(b, opt) != 0x20b) throw new Exception("Expected an x64 PE DLL");
            if ((BitConverter.ToUInt16(b, pe + 22) & 0x2000) == 0) throw new Exception("PE is not a DLL");
            int exp = Rva(b, pe, U(b, opt + 112));
            int names = Rva(b, pe, U(b, exp + 32));
            int ordinals = Rva(b, pe, U(b, exp + 36));
            int functions = Rva(b, pe, U(b, exp + 28));
            int declaration = -1;
            bool load = false, version = false;
            for (uint i = 0; i < U(b, exp + 24); ++i) {
                string name = Z(b, Rva(b, pe, U(b, names + checked((int)i) * 4)));
                load |= name == "SKSEPlugin_Load"; version |= name == "SKSEPlugin_Version";
                if (name == "SKSEPlugin_Version") {
                    int ordinal = BitConverter.ToUInt16(b, ordinals + checked((int)i) * 2);
                    declaration = Rva(b, pe, U(b, functions + ordinal * 4));
                }
            }
            if (!load || !version) throw new Exception("Required SKSE exports are missing");
            var expected = new Version(expectedVersion);
            uint packedVersion = ((uint)expected.Major << 24) | ((uint)expected.Minor << 16) | ((uint)expected.Build << 4);
            if (U(b, declaration) != 1 || U(b, declaration + 4) != packedVersion)
                throw new Exception("SKSE declaration version does not match the project");
            // PluginDeclarationInfo begins four bytes after its structure version.
            // NG supports both structure families and Address Library formats 1/2/5.
            if (U(b, declaration + 0x304) != 3 || U(b, declaration + 0x308) != 1)
                throw new Exception("SKSE declaration must advertise NG and Address Library v5 support");
            for (int i = 0; i < 16; ++i)
                if (U(b, declaration + 0x30C + i * 4) != 0)
                    throw new Exception("SKSE Address Library declaration contains a fixed runtime list");
            // Reject accidental debug CRT linkage. Record remaining imports in the report.
            var imports = new System.Collections.Generic.List<string>();
            int imp = Rva(b, pe, U(b, opt + 120));
            for (; U(b, imp + 12) != 0; imp += 20) {
                string name = Z(b, Rva(b, pe, U(b, imp + 12)));
                string lower = name.ToLowerInvariant();
                if (lower == "ucrtbased.dll" || lower.EndsWith("140d.dll") || lower.EndsWith("140_1d.dll"))
                    throw new Exception("Debug runtime import: " + name);
                imports.Add(name);
            }
            int debug = Rva(b, pe, U(b, opt + 160));
            uint debugSize = U(b, opt + 164), age = 0;
            Guid guid = Guid.Empty;
            string symbolPath = null;
            for (int i = 0; i < debugSize; i += 28) {
                if (U(b, debug + i + 12) != 2) continue;
                int cv = checked((int)U(b, debug + i + 24));
                if (U(b, cv) != 0x53445352) continue;
                guid = GuidAt(b, cv + 4); age = U(b, cv + 20);
                symbolPath = Z(b, cv + 24); break;
            }
            if (guid == Guid.Empty) throw new Exception("DLL has no RSDS symbol identity");
            if (symbolPath != Path.GetFileName(pdb))
                throw new Exception("DLL must reference the matching PDB by filename only");
            byte[] p = File.ReadAllBytes(pdb);
            if (!Encoding.ASCII.GetString(p, 0, 24).StartsWith("Microsoft C/C++ MSF 7.00"))
                throw new Exception("Expected an MSF 7 PDB");
            int block = checked((int)U(p, 32)), dirSize = checked((int)U(p, 44));
            int map = checked((int)U(p, 52) * block);
            byte[] dir = new byte[dirSize];
            for (int i = 0; i * block < dirSize; ++i)
                Array.Copy(p, checked((int)U(p, map + i * 4) * block), dir, i * block,
                           Math.Min(block, dirSize - i * block));
            int streams = checked((int)U(dir, 0));
            if (streams < 2 || U(dir, 8) < 28 || U(dir, 8) == uint.MaxValue)
                throw new Exception("PDB info stream is missing");
            uint firstSize = U(dir, 4);
            int firstBlocks = firstSize == uint.MaxValue ? 0 : checked((int)((firstSize + block - 1) / block));
            int info = checked((int)U(dir, 4 + streams * 4 + firstBlocks * 4) * block);
            if (U(p, info + 8) != age || GuidAt(p, info + 12) != guid)
                throw new Exception("PDB GUID/age does not match the DLL");
            return "x64; SKSE version " + expectedVersion + "; SE/AE NG, Address Library v5; matching PDB " + symbolPath + " " + guid + " age " + age +
                   "; imports: " + String.Join(", ", imports.ToArray());
        }
    }
}
'@
}
[DdcRelease.BinaryCheck]::Verify((Resolve-Path -LiteralPath $Dll).Path, (Resolve-Path -LiteralPath $Pdb).Path, $Version)
