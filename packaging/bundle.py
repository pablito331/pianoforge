#!/usr/bin/env python3
"""PianoForge bundle format (.pforge) — pack / unpack / list / verify.

Layout binário (little-endian):

    offset 0   magic       4B   b"PFG1"
    offset 4   version     u16  = 1
    offset 6   flags       u16  (reservado, 0)
    offset 8   ndir        u32  quantidade de entradas de diretório
    offset 12  diretório:
        para cada entrada:
            name_len  u16
            name      bytes (utf-8, caminho relativo tipo "bank/sound.sf2")
            type      u16   0=blob cru, 1=blob zlib, 2=manifest JSON (utf-8)
            offset    u64   deslocamento absoluto do payload no arquivo
            size      u64   tamanho do payload gravado
            usize     u64   tamanho original (== size quando type 0/2)
            crc32     u32   CRC32 (zlib) do payload gravado — detecta corrupção
    ...       payloads (blocos consecutivos, offset já resolvido)

O manifest ("manifest.json", type 2) referencia os demais entries por
`path`. Créditos/licença dos bancos ficam em manifest.credits — nunca
remova atribuição exigida por licença CC-BY.

Uso:
    bundle.py pack   <out.pforge> <manifest.json> <arquivo...>
    bundle.py unpack <in.pforge>  <outdir>
    bundle.py list   <in.pforge>
    bundle.py verify <in.pforge>
"""
import json
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"PFG1"
VERSION = 1
T_BLOB, T_ZLIB, T_MANIFEST = 0, 1, 2


def pack(out: Path, manifest_path: Path, files: list[Path]):
    manifest = json.loads(manifest_path.read_text("utf-8"))
    # entries: manifest primeiro, depois os arquivos passados
    entries = [("manifest.json", T_MANIFEST, manifest_path.read_bytes())]
    seen = {e[0] for e in entries}
    for f in files:
        # path relativo = o que está referenciado no manifest (basename por padrão)
        rel = f.name
        if rel in seen:
            sys.exit(f"path duplicado no bundle: {rel}")
        seen.add(rel)
        raw = f.read_bytes()
        # zlib só compensa em texto/já-não-comprimido; testa e guarda o que for menor
        comp = zlib.compress(raw, 6)
        if len(comp) < len(raw) * 0.95:
            entries.append((rel, T_ZLIB, comp))
        else:
            entries.append((rel, T_BLOB, raw))

    ndir = len(entries)
    dir_buf = bytearray()
    # primeiro passa: tamanho do diretório pra saber offsets
    dir_size = sum(2 + len(n.encode()) + 2 + 24 + 4 for n, _, _ in entries)
    header_size = 4 + 2 + 2 + 4 + dir_size
    off = header_size
    payloads = []
    for name, typ, payload in entries:
        nb = name.encode("utf-8")
        usize = len(payload) if typ != T_ZLIB else len(zlib.decompress(payload))
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        dir_buf += struct.pack("<H", len(nb)) + nb
        dir_buf += struct.pack("<HQQQI", typ, off, len(payload), usize, crc)
        payloads.append(payload)
        off += len(payload)

    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<HHI", VERSION, 0, ndir))
        f.write(dir_buf)
        for p in payloads:
            f.write(p)
    print(f"pack: {out} ({off:,} bytes, {ndir} entries)")


def read_dir(path: Path):
    data = path.read_bytes()
    if data[:4] != MAGIC:
        sys.exit(f"magic inválido: {data[:4]!r}")
    version, flags, ndir = struct.unpack_from("<HHI", data, 4)
    if version != VERSION:
        sys.exit(f"versão não suportada: {version}")
    pos = 12
    entries = []
    for _ in range(ndir):
        (nlen,) = struct.unpack_from("<H", data, pos)
        pos += 2
        name = data[pos : pos + nlen].decode("utf-8")
        pos += nlen
        typ, off, size, usize, crc = struct.unpack_from("<HQQQI", data, pos)
        pos += 30  # H(2) + Q(8) + Q(8) + Q(8) + I(4)
        entries.append({"name": name, "type": typ, "off": off, "size": size, "usize": usize, "crc": crc})
    return data, entries


def payload(data: bytes, e: dict) -> bytes:
    raw = data[e["off"] : e["off"] + e["size"]]
    if zlib.crc32(raw) & 0xFFFFFFFF != e["crc"]:
        raise ValueError(f"CRC inválido em {e['name']}")
    if e["type"] == T_ZLIB:
        raw = zlib.decompress(raw)
        if len(raw) != e["usize"]:
            raise ValueError(f"usize inconsistente em {e['name']}")
    return raw


def unpack(inp: Path, outdir: Path):
    data, entries = read_dir(inp)
    outdir.mkdir(parents=True, exist_ok=True)
    for e in entries:
        dest = outdir / e["name"]
        if not dest.resolve().is_relative_to(outdir.resolve()):
            sys.exit(f"path suspeito no bundle: {e['name']}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            dest.write_bytes(payload(data, e))
        except ValueError as ex:
            sys.exit(f"FALHA ao extrair {e['name']}: {ex}")
        print(f"  {e['name']}  ({e['usize']:,} bytes)")
    print(f"unpack: {len(entries)} entries -> {outdir}")


def listdir(inp: Path):
    data, entries = read_dir(inp)
    m = json.loads(payload(data, next(e for e in entries if e["type"] == T_MANIFEST)))
    print(f"produto: {m.get('produto','?')} v{m.get('versao','?')}")
    for e in entries:
        flag = {0: "raw", 1: "zlib", 2: "json"}[e["type"]]
        print(f"  {flag:4s} {e['name']:36s} {e['usize']:>13,}")
    for p in m.get("presets", []):
        print(f"  preset: {p.get('nome')}  params={len(p.get('params',{}))}  img={p.get('imagem','-')}")
    for c in m.get("credits", []):
        print(f"  credit: {c.get('autor','?')} — {c.get('licenca','?')} ({c.get('fonte','-')})")


def verify(inp: Path):
    data, entries = read_dir(inp)
    ok = True
    for e in entries:
        if e["off"] + e["size"] > len(data):
            print(f"FALHA: {e['name']} fora do arquivo")
            ok = False
            continue
        try:
            p = payload(data, e)
            if e["type"] == T_MANIFEST:
                json.loads(p)  # deve ser JSON válido
            print(f"  ok  {e['name']}")
        except Exception as ex:  # noqa: BLE001 — relatório de verificação
            print(f"  FALHA {e['name']}: {ex}")
            ok = False
    # manifest deve referenciar todos os blobs
    m = json.loads(payload(data, next(e for e in entries if e["type"] == T_MANIFEST)))
    names = {e["name"] for e in entries}
    def walk_refs(obj):
        if isinstance(obj, dict):
            for k, v in obj.items():
                if k in ("imagem", "sf2") and isinstance(v, str):
                    if v and v not in names:
                        print(f"  FALHA: manifest referencia {v} inexistente")
                        return False
                    continue
                if not walk_refs(v):
                    return False
            return True
        elif isinstance(obj, list):
            for v in obj:
                if not walk_refs(v):
                    return False
        return True
    if not walk_refs(m):
        ok = False
    print("verify:", "OK" if ok else "FALHAS")
    return ok


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, rest = sys.argv[1], sys.argv[2:]
    if cmd == "pack":
        if len(rest) < 3:
            sys.exit("pack <out.pforge> <manifest.json> <arquivos...>")
        pack(Path(rest[0]), Path(rest[1]), [Path(x) for x in rest[2:]])
    elif cmd == "unpack":
        unpack(Path(rest[0]), Path(rest[1]))
    elif cmd == "list":
        listdir(Path(rest[0]))
    elif cmd == "verify":
        sys.exit(0 if verify(Path(rest[0])) else 1)
    else:
        sys.exit(f"comando desconhecido: {cmd}")


if __name__ == "__main__":
    main()
