// Independent fixtures using Node's crypto and noble secp256k1 (test identities only).
const fs=require('fs');
const path=require('path');
const crypto=require('crypto');
const modules=process.env.NOSTR_ORACLE_MODULES || path.resolve('node_modules');
const {schnorr,secp256k1}=require(path.join(modules,'@noble/curves/secp256k1.js'));
const key=n=>Buffer.from(n.toString(16).padStart(64,'0'),'hex');
const pub=sk=>Buffer.from(schnorr.getPublicKey(sk)).toString('hex');
const alice=key(1),bob=key(2),carol=key(3),ephemeral=key(7);
const hash=e=>crypto.createHash('sha256').update(JSON.stringify([0,e.pubkey,e.created_at,e.kind,e.tags,e.content])).digest('hex');
const hmac=(key,data)=>crypto.createHmac('sha256',key).update(data).digest();
function conversation(sk,peer) {
  const point=secp256k1.ProjectivePoint.fromHex('02'+peer).multiply(BigInt('0x'+sk.toString('hex')));
  return hmac(Buffer.from('nip44-v2'),point.toRawBytes(false).slice(1,33));
}
function expand(key,nonce) {
  let previous=Buffer.alloc(0);const blocks=[];
  for(let i=1;i<=3;++i){previous=hmac(key,Buffer.concat([previous,nonce,Buffer.from([i])]));blocks.push(previous);}
  return Buffer.concat(blocks).subarray(0,76);
}
function padded(length) {if(length<=32)return 32;const power=2**Math.ceil(Math.log2(length));const chunk=power<=256?32:power/8;return chunk*Math.ceil(length/chunk);}
function encrypt(sk,peer,text,nonce=crypto.randomBytes(32)) {
  const keys=expand(conversation(sk,peer),nonce),bytes=Buffer.from(text);
  const plain=Buffer.alloc(2+padded(bytes.length));plain.writeUInt16BE(bytes.length);bytes.copy(plain,2);
  const cipher=crypto.createCipheriv('chacha20',keys.subarray(0,32),Buffer.concat([Buffer.alloc(4),keys.subarray(32,44)]));
  const content=Buffer.concat([cipher.update(plain),cipher.final()]);
  return Buffer.concat([Buffer.from([2]),nonce,content,hmac(keys.subarray(44),Buffer.concat([nonce,content]))]).toString('base64');
}
function decrypt(sk,peer,text) {
  const payload=Buffer.from(text,'base64'),nonce=payload.subarray(1,33),keys=expand(conversation(sk,peer),nonce);
  const content=payload.subarray(33,-32);
  if(!crypto.timingSafeEqual(payload.subarray(-32),hmac(keys.subarray(44),Buffer.concat([nonce,content]))))throw Error('MAC mismatch');
  const cipher=crypto.createDecipheriv('chacha20',keys.subarray(0,32),Buffer.concat([Buffer.alloc(4),keys.subarray(32,44)]));
  const plain=Buffer.concat([cipher.update(content),cipher.final()]);return plain.subarray(2,2+plain.readUInt16BE()).toString();
}
function sign(e,sk) {e.id=hash(e);e.sig=Buffer.from(schnorr.sign(e.id,sk)).toString('hex');return e;}
function wrap(rumor, sealTags=[], editSeal=()=>{}, outerTags=[]) {
  const seal=sign({pubkey:pub(alice),created_at:1789999000,kind:13,tags:sealTags,content:encrypt(alice,pub(bob),JSON.stringify(rumor))},alice);
  editSeal(seal);
  return sign({pubkey:pub(ephemeral),created_at:1789998000,kind:1059,tags:[['p',pub(bob)],...outerTags],content:encrypt(ephemeral,pub(bob),JSON.stringify(seal))},ephemeral);
}
const rumor={pubkey:pub(alice),created_at:1790000000,kind:14,tags:[['p',pub(bob)]],content:'Independent JS message'};rumor.id=hash(rumor);
const impersonation={...rumor,pubkey:pub(carol)};impersonation.id=hash(impersonation);
const group={...rumor,tags:[['p',pub(bob)],['p',pub(carol)]]};group.id=hash(group);
const wrongRecipient={...rumor,tags:[['p',pub(alice)]]};wrongRecipient.id=hash(wrongRecipient);
const wrongId={...rumor,id:'0'.repeat(64)};
const fixedNonce=Buffer.alloc(32);fixedNonce[31]=1;
const vectors=[1,32,33,256,257,512,513,1024,4096].map(size=>({size,payload:encrypt(alice,pub(bob),'a'.repeat(size),fixedNonce)}));
fs.writeFileSync('.build-cache/modern/oracle.json',JSON.stringify({valid:wrap(rumor),impersonation:wrap(impersonation),group:wrap(group),wrongId:wrap(wrongId),wrongRecipient:wrap(wrongRecipient),vectors,
  expiring:wrap(rumor,[['expiration','4294967295']]),
  expired:wrap(rumor,[['expiration','1']]),
  badExpiration:wrap(rumor,[['expiration','4294967296']]),
  unsupportedSealTag:wrap(rumor,[['p',pub(bob)]]),
  sealMissingId:wrap(rumor,[],seal=>delete seal.id),
  sealWrongId:wrap(rumor,[],seal=>seal.id='0'.repeat(64)),
  sealBadSignature:wrap(rumor,[],seal=>seal.sig='0'.repeat(128)),
  sealWrongKind:wrap(rumor,[],seal=>{seal.kind=14;sign(seal,alice);}),
  sealTamperedTags:wrap(rumor,[],seal=>seal.tags=[['expiration','4294967295']]),
  outerExpired:wrap(rumor,[],()=>{},[['expiration','1']])
}));
if(process.argv.includes('--check')) {
  const outer=JSON.parse(fs.readFileSync('.build-cache/modern/firmware-wrap.json','utf8'));
  if(!schnorr.verify(outer.sig,hash(outer),outer.pubkey))throw Error('Invalid wrapper signature');
  const seal=JSON.parse(decrypt(bob,outer.pubkey,outer.content));
  if(!schnorr.verify(seal.sig,hash(seal),seal.pubkey))throw Error('Invalid seal signature');
  const message=JSON.parse(decrypt(bob,seal.pubkey,seal.content));
  if(message.pubkey!==seal.pubkey || message.id!==hash(message) || message.content!=='Hello from the pager')throw Error('Invalid rumor');
  console.log('Independent Node/OpenSSL/noble oracle decrypts and authenticates firmware gift wrap.');
}
