"""
Genera trust_anchors.h compatible con SSLClient (BearSSL) a partir
de uno o varios certificados PEM (solo se procesan los CA / self-signed).

Uso:
    python gen_trust_anchor.py <cert.pem>  [output.h]

Si se omite output.h, escribe en stdout.
"""
import sys
import textwrap
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import rsa, ec
from cryptography.hazmat.primitives.serialization import Encoding
from cryptography.hazmat.backends import default_backend

# ── helpers ────────────────────────────────────────────────────────────────────

def to_c_array(name: str, data: bytes) -> str:
    hex_bytes = ", ".join(f"0x{b:02x}" for b in data)
    wrapped = textwrap.fill(hex_bytes, width=72, subsequent_indent="  ")
    return f"static const unsigned char {name}[] = {{\n  {wrapped}\n}};\n"

def dn_der(cert: x509.Certificate) -> bytes:
    """DER-encoded subject DN."""
    return cert.subject.public_bytes(default_backend())

def rsa_trust_anchor(cert: x509.Certificate, idx: int) -> str:
    pub = cert.public_key()
    if not isinstance(pub, rsa.RSAPublicKey):
        raise ValueError("Cert is not RSA")
    nums = pub.public_numbers()
    modulus = nums.n.to_bytes((nums.n.bit_length() + 7) // 8, "big")
    exponent = nums.e.to_bytes((nums.e.bit_length() + 7) // 8, "big")
    dn = dn_der(cert)

    lines = []
    lines.append(to_c_array(f"TA{idx}_DN", dn))
    lines.append(to_c_array(f"TA{idx}_RSA_N", modulus))
    lines.append(to_c_array(f"TA{idx}_RSA_E", exponent))

    lines.append(f"""#define TA{idx} {{ \\
  {{ (unsigned char *)TA{idx}_DN, sizeof TA{idx}_DN }}, \\
  BR_X509_TA_CA, \\
  {{ .rsa = {{ \\
    (unsigned char *)TA{idx}_RSA_N, sizeof TA{idx}_RSA_N, \\
    (unsigned char *)TA{idx}_RSA_E, sizeof TA{idx}_RSA_E, \\
  }} }}, \\
}}
""")
    return "\n".join(lines)

def ec_trust_anchor(cert: x509.Certificate, idx: int) -> str:
    pub = cert.public_key()
    if not isinstance(pub, ec.EllipticCurvePublicKey):
        raise ValueError("Cert is not EC")
    curve = pub.curve
    # BearSSL curve ID
    curve_map = {
        "secp256r1": "BR_EC_secp256r1",
        "secp384r1": "BR_EC_secp384r1",
        "secp521r1": "BR_EC_secp521r1",
    }
    br_curve = curve_map.get(curve.name, f"/* unknown curve: {curve.name} */")
    pub_bytes = pub.public_bytes(Encoding.X962,
                                  __import__("cryptography.hazmat.primitives.serialization",
                                             fromlist=["PublicFormat"]).PublicFormat.UncompressedPoint)
    dn = dn_der(cert)

    lines = []
    lines.append(to_c_array(f"TA{idx}_DN", dn))
    lines.append(to_c_array(f"TA{idx}_EC_Q", pub_bytes))

    lines.append(f"""#define TA{idx} {{ \\
  {{ (unsigned char *)TA{idx}_DN, sizeof TA{idx}_DN }}, \\
  BR_X509_TA_CA, \\
  {{ .ec = {{ \\
    {br_curve}, \\
    (unsigned char *)TA{idx}_EC_Q, sizeof TA{idx}_EC_Q, \\
  }} }}, \\
}}
""")
    return "\n".join(lines)

# ── main ───────────────────────────────────────────────────────────────────────

def load_pem_certs(path: str):
    with open(path, "rb") as f:
        data = f.read()
    certs = []
    marker = b"-----BEGIN CERTIFICATE-----"
    end    = b"-----END CERTIFICATE-----"
    while marker in data:
        start = data.index(marker)
        stop  = data.index(end, start) + len(end)
        pem_block = data[start:stop]
        certs.append(x509.load_pem_x509_certificate(pem_block, default_backend()))
        data = data[stop:]
    return certs

def is_ca(cert: x509.Certificate) -> bool:
    try:
        bc = cert.extensions.get_extension_for_class(x509.BasicConstraints)
        return bc.value.ca
    except x509.ExtensionNotFound:
        return False

def generate(pem_path: str) -> str:
    certs = load_pem_certs(pem_path)
    ca_certs = [c for c in certs if is_ca(c)]
    if not ca_certs:
        print("[WARN] No CA certs found — using all certs", file=sys.stderr)
        ca_certs = certs

    lines = [
        "// trust_anchors.h — generado automáticamente con gen_trust_anchor.py",
        "// NO editar a mano. Regenerar si el cert CA cambia.",
        "// Compatible con SSLClient (OPEnSLab-OSU/SSLClient, BearSSL).",
        "#pragma once",
        '#include "bearssl.h"',
        "",
    ]

    ta_macros = []
    for i, cert in enumerate(ca_certs):
        subject = cert.subject.rfc4514_string()
        lines.append(f"// ── Trust anchor {i}: {subject}")
        pub = cert.public_key()
        if isinstance(pub, rsa.RSAPublicKey):
            lines.append(rsa_trust_anchor(cert, i))
        elif isinstance(pub, ec.EllipticCurvePublicKey):
            lines.append(ec_trust_anchor(cert, i))
        else:
            print(f"[WARN] TA{i}: unsupported key type — skipped", file=sys.stderr)
            continue
        ta_macros.append(f"TA{i}")

    if not ta_macros:
        raise RuntimeError("No valid trust anchors generated")

    ta_list = ",\n  ".join(ta_macros)
    lines.append(f"static const br_x509_trust_anchor TAs[{len(ta_macros)}] = {{")
    lines.append(f"  {ta_list},")
    lines.append("};")
    lines.append(f"\n#define TAs_NUM {len(ta_macros)}")
    return "\n".join(lines) + "\n"

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    pem_path = sys.argv[1]
    output   = sys.argv[2] if len(sys.argv) > 2 else None
    result = generate(pem_path)
    if output:
        with open(output, "w", encoding="utf-8") as f:
            f.write(result)
        print(f"[OK] Escrito en {output}")
    else:
        print(result)
