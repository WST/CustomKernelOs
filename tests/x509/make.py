# tests/x509/make.py OUTDIR — тестовые цепочки сертификатов и подписи (нужен python3-cryptography)
import sys, os, datetime
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
now = datetime.datetime.now(datetime.timezone.utc)
def name(cn, org=None):
    a = [x509.NameAttribute(NameOID.COMMON_NAME, cn)]
    if org: a.insert(0, x509.NameAttribute(NameOID.ORGANIZATION_NAME, org))
    return x509.Name(a)
def mk(subject, key, issuer, ikey, ca, hashalg, sans=None, days=(-1, 365)):
    b = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now + datetime.timedelta(days=days[0]))
         .not_valid_after(now + datetime.timedelta(days=days[1]))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if sans: b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(s) for s in sans]), critical=False)
    return b.sign(ikey, hashalg)
def der(c): return c.public_bytes(serialization.Encoding.DER)
root_rsa = rsa.generate_private_key(public_exponent=65537, key_size=4096)
root_ec = ec.generate_private_key(ec.SECP384R1())
inter_ec = ec.generate_private_key(ec.SECP384R1())
inter_rsa = rsa.generate_private_key(public_exponent=65537, key_size=2048)
leaf_ec = ec.generate_private_key(ec.SECP256R1())
leaf_rsa = rsa.generate_private_key(public_exponent=65537, key_size=2048)
rootA = mk(name("MyOS Test Root RSA", "MyOS Test"), root_rsa, name("MyOS Test Root RSA", "MyOS Test"), root_rsa, True, hashes.SHA256(), days=(-10, 3650))
rootB = mk(name("MyOS Test Root EC", "MyOS Test"), root_ec, name("MyOS Test Root EC", "MyOS Test"), root_ec, True, hashes.SHA384(), days=(-10, 3650))
interA = mk(name("MyOS Test CA EC"), inter_ec, rootA.subject, root_rsa, True, hashes.SHA384())
interB = mk(name("MyOS Test CA RSA"), inter_rsa, rootB.subject, root_ec, True, hashes.SHA384())
sans = ["test.myos", "*.example.com", "example.com", "localhost"]
leafA = mk(name("example.com"), leaf_ec, interA.subject, inter_ec, False, hashes.SHA256(), sans)      # EC P-256 <- EC P-384 <- RSA4096
leafB = mk(name("example.com"), leaf_rsa, interB.subject, inter_rsa, False, hashes.SHA256(), sans)    # RSA <- RSA <- EC P-384
expired = mk(name("example.com"), leaf_ec, interA.subject, inter_ec, False, hashes.SHA256(), sans, days=(-100, -5))
selfsigned = mk(name("example.com"), leaf_ec, name("example.com"), leaf_ec, False, hashes.SHA256(), sans)
open(os.path.join(out, "roots.der"), "wb").write(der(rootA) + der(rootB))
for nm, cs in (("chainA", [leafA, interA]), ("chainB", [leafB, interB]), ("expired", [expired, interA]), ("self", [selfsigned]),
               ("nointer", [leafA]), ("swapped", [leafA, interB])):
    with open(os.path.join(out, nm + ".chain"), "wb") as f:
        for c in cs:
            d = der(c); f.write(len(d).to_bytes(4, "big") + d)
# подписи как в TLS 1.3 CertificateVerify
msg = b" " * 64 + b"TLS 1.3, server CertificateVerify\0" + bytes(range(32))
open(os.path.join(out, "msg.bin"), "wb").write(msg)
sigs = [("0403", leafA, leaf_ec.sign(msg, ec.ECDSA(hashes.SHA256()))),
        ("0804", leafB, leaf_rsa.sign(msg, padding.PSS(padding.MGF1(hashes.SHA256()), 32), hashes.SHA256())),
        ("0805", leafB, leaf_rsa.sign(msg, padding.PSS(padding.MGF1(hashes.SHA384()), 48), hashes.SHA384())),
        ("0401", leafB, leaf_rsa.sign(msg, padding.PKCS1v15(), hashes.SHA256())),
        ("0503", interA, inter_ec.sign(msg, ec.ECDSA(hashes.SHA384())))]
with open(os.path.join(out, "sigs.txt"), "w") as f:
    for i, (sch, c, s) in enumerate(sigs):
        open(os.path.join(out, "sig%d.bin" % i), "wb").write(s)
        open(os.path.join(out, "sig%d.der" % i), "wb").write(der(c))
        f.write("%s sig%d\n" % (sch, i))
# сервер для проверки в QEMU: ключи и цепочки в PEM
pem = lambda c: c.public_bytes(serialization.Encoding.PEM)
kpem = lambda k: k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption())
open(os.path.join(out, "serverA.pem"), "wb").write(pem(leafA) + pem(interA)); open(os.path.join(out, "serverA.key"), "wb").write(kpem(leaf_ec))
open(os.path.join(out, "serverB.pem"), "wb").write(pem(leafB) + pem(interB)); open(os.path.join(out, "serverB.key"), "wb").write(kpem(leaf_rsa))
open(os.path.join(out, "expired.pem"), "wb").write(pem(expired) + pem(interA))
open(os.path.join(out, "self.pem"), "wb").write(pem(selfsigned))
