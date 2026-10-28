// Warcraft III .mdl (text) reader: tokenizer + generic block tree, converted to the structure of
// ModelMdxData.h; the model is then built by the same code as .mdx (ModelMdx.cpp).
#include "ModelMdxData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rts::render::mdx {

namespace {

// Generic tree: an item is the tokens up to ',' plus an optional { block } of items.
struct MItem {
    std::vector<std::string> head;
    std::vector<MItem> kids;
    bool block = false;
    int line = 0;
};

class MdlReader {
public:
    MdlReader(const std::string& path, const std::string& text) : path_(path), s_(text) {}

    Mdlx parse() {
        std::vector<MItem> top = items(0);
        if (tok_.type != Tok::End) fail("unexpected '}'");
        for (const MItem& it : top) section(it);
        return std::move(m_);
    }

private:
    struct Tok { enum Type { End, Word, Str, Open, Close, Comma, Colon } type = End; std::string text; };

    [[noreturn]] void fail(const std::string& what) const { throw std::runtime_error(path_ + ":" + std::to_string(line_) + ": " + what); }

    void next() {
        for (;;) {
            while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) { if (s_[p_] == '\n') ++line_; ++p_; }
            if (p_ + 1 < s_.size() && s_[p_] == '/' && s_[p_ + 1] == '/') { while (p_ < s_.size() && s_[p_] != '\n') ++p_; continue; }
            break;
        }
        tok_.text.clear();
        if (p_ >= s_.size()) { tok_.type = Tok::End; return; }
        const char c = s_[p_];
        if (c == '{') { tok_.type = Tok::Open; ++p_; return; }
        if (c == '}') { tok_.type = Tok::Close; ++p_; return; }
        if (c == ',') { tok_.type = Tok::Comma; ++p_; return; }
        if (c == ':') { tok_.type = Tok::Colon; ++p_; return; }
        if (c == '"') {
            const size_t e = s_.find('"', p_ + 1);
            if (e == std::string::npos) fail("unterminated string");
            tok_.type = Tok::Str;
            tok_.text = s_.substr(p_ + 1, e - p_ - 1);
            for (char ch : tok_.text) if (ch == '\n') ++line_;
            p_ = e + 1;
            return;
        }
        const size_t b = p_;
        while (p_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[p_])) && !std::strchr("{},:\"", s_[p_])) ++p_;
        tok_.type = Tok::Word;
        tok_.text = s_.substr(b, p_ - b);
    }

    std::vector<MItem> items(int depth) {
        if (depth > 32) fail("blocks nested too deeply");
        std::vector<MItem> out;
        MItem cur;
        cur.line = line_;
        auto flush = [&] { if (!cur.head.empty() || cur.block) out.push_back(std::move(cur)); cur = MItem{}; cur.line = line_; };
        for (next();; next()) {
            switch (tok_.type) {
                case Tok::End: flush(); return out;
                case Tok::Close: flush(); return out;
                case Tok::Comma: flush(); break;
                case Tok::Colon: cur.head.push_back(":"); break;
                case Tok::Word: case Tok::Str: cur.head.push_back(tok_.text); break;
                case Tok::Open:
                    cur.kids = items(depth + 1);
                    if (tok_.type != Tok::Close) fail("missing '}'");
                    cur.block = true;
                    flush();
                    break;
            }
        }
    }

    float num(const std::string& t) const {
        char* e = nullptr;
        const float v = std::strtof(t.c_str(), &e);
        if (e == t.c_str()) fail("number expected, got '" + t + "'");
        return std::isfinite(v) ? v : 0.0f;
    }
    int inum(const std::string& t) const { return int(std::lround(num(t))); }
    // numbers of an item: head tokens from `from`, then everything inside its block (recursively)
    void collect(const MItem& it, size_t from, std::vector<float>& out) const {
        for (size_t i = from; i < it.head.size(); ++i) if (it.head[i] != ":") out.push_back(num(it.head[i]));
        for (const MItem& k : it.kids) collect(k, 0, out);
    }
    std::vector<float> nums(const MItem& it, size_t from = 1) const { std::vector<float> v; collect(it, from, v); return v; }
    static bool is(const MItem& it, const char* key) { return !it.head.empty() && it.head[0] == key; }
    static const std::string& arg(const MItem& it, size_t i) { static const std::string empty; return i < it.head.size() ? it.head[i] : empty; }

    // Keyframes: "Linear," "GlobalSeqId n," then "frame: value," with optional InTan / OutTan items.
    Track track(const MItem& it, int comps) {
        line_ = it.line;
        Track t;
        t.comps = comps;
        for (const MItem& k : it.kids) {
            line_ = k.line;
            if (k.head.empty()) continue;
            const std::string& h = k.head[0];
            if (h == "DontInterp") t.interp = 0;
            else if (h == "Linear") t.interp = 1;
            else if (h == "Hermite") t.interp = 2;
            else if (h == "Bezier") t.interp = 3;
            else if (h == "GlobalSeqId") t.globalSeq = inum(arg(k, 1));
            else if (h == "InTan" || h == "OutTan") {
                std::vector<float> v = nums(k);
                v.resize(size_t(comps), 0.0f);
                auto& dst = h == "InTan" ? t.inTans : t.outTans;
                dst.resize(t.frames.empty() ? 0 : (t.frames.size() - 1) * size_t(comps));
                dst.insert(dst.end(), v.begin(), v.end());
            } else if (k.head.size() >= 2 && k.head[1] == ":") {
                t.frames.push_back(inum(h));
                std::vector<float> v = nums(k, 2);
                if (v.size() < size_t(comps)) fail("keyframe needs " + std::to_string(comps) + " values");
                t.values.insert(t.values.end(), v.begin(), v.begin() + comps);
            }
        }
        if (t.interp > 1) {
            t.inTans.resize(t.values.size(), 0.0f);
            t.outTans.resize(t.values.size(), 0.0f);
        }
        return t;
    }

    void section(const MItem& it) {
        line_ = it.line;
        if (it.head.empty()) return;
        const std::string& h = it.head[0];
        if (h == "Version") {
            for (const MItem& k : it.kids) if (is(k, "FormatVersion")) m_.version = uint32_t(inum(arg(k, 1)));
        } else if (h == "Sequences") {
            for (const MItem& k : it.kids) {
                if (!is(k, "Anim")) continue;
                Sequence s;
                s.name = arg(k, 1);
                for (const MItem& a : k.kids) {
                    if (is(a, "Interval")) { std::vector<float> v = nums(a); if (v.size() >= 2) { s.start = int32_t(v[0]); s.end = int32_t(v[1]); } }
                    else if (is(a, "NonLooping")) s.nonLooping = true;
                }
                m_.seqs.push_back(s);
            }
        } else if (h == "GlobalSequences") {
            for (const MItem& k : it.kids) if (is(k, "Duration")) m_.globals.push_back(inum(arg(k, 1)));
        } else if (h == "Textures") {
            for (const MItem& k : it.kids) {
                if (!is(k, "Bitmap")) continue;
                Texture t;
                for (const MItem& a : k.kids) {
                    if (is(a, "Image")) t.file = arg(a, 1);
                    else if (is(a, "ReplaceableId")) t.replaceable = uint32_t(inum(arg(a, 1)));
                    else if (is(a, "WrapWidth")) t.flags |= 1;
                    else if (is(a, "WrapHeight")) t.flags |= 2;
                }
                m_.textures.push_back(t);
            }
        } else if (h == "Materials") {
            for (const MItem& k : it.kids) if (is(k, "Material")) material(k);
        } else if (h == "Geoset") geoset(it);
        else if (h == "GeosetAnim") {
            GeosetAnim a;
            for (const MItem& k : it.kids) {
                if (is(k, "static") && arg(k, 1) == "Alpha") a.alpha = num(arg(k, 2));
                else if (is(k, "Alpha")) a.alphaTrack = track(k, 1);
                else if (is(k, "GeosetId")) a.geoset = inum(arg(k, 1));
            }
            m_.geoAnims.push_back(std::move(a));
        } else if (h == "Bone" || h == "Helper" || h == "Attachment" || h == "Light" || h == "ParticleEmitter" || h == "ParticleEmitter2" ||
                   h == "ParticleEmitterPopcorn" || h == "RibbonEmitter" || h == "EventObject" || h == "CollisionShape") {
            Object o;
            o.kind = h == "Bone" ? Object::Bone : h == "Helper" ? Object::Helper : h == "Attachment" ? Object::Attachment : Object::Other;
            if (o.kind == Object::Other && h != "EventObject" && h != "CollisionShape") m_.emitters = true;
            o.name = arg(it, 1);
            o.id = -2;   // assigned by order when missing
            for (const MItem& k : it.kids) {
                line_ = k.line;
                if (is(k, "ObjectId")) o.id = inum(arg(k, 1));
                else if (is(k, "Parent")) o.parent = inum(arg(k, 1));
                else if (is(k, "Translation")) o.t = track(k, 3);
                else if (is(k, "Rotation")) o.r = track(k, 4);
                else if (is(k, "Scaling")) o.s = track(k, 3);
                else if (is(k, "Billboarded") || is(k, "BillboardedLockX") || is(k, "BillboardedLockY") || is(k, "BillboardedLockZ") || is(k, "CameraAnchored")) o.flags |= 8;
                else if (is(k, "DontInherit")) o.flags |= 1;
            }
            if (o.id == -2) o.id = int32_t(m_.objects.size());
            m_.objects.push_back(std::move(o));
        } else if (h == "PivotPoints") {
            for (const MItem& k : it.kids) {
                std::vector<float> v = nums(k, 0);
                if (v.size() >= 3) m_.pivots.push_back({v[0], v[1], v[2]});
            }
        }
    }

    static uint32_t filterMode(const std::string& f) {
        static const char* names[] = {"None", "Transparent", "Blend", "Additive", "AddAlpha", "Modulate", "Modulate2x"};
        for (uint32_t i = 0; i < 7; ++i) if (f == names[i]) return i;
        return 0;
    }

    void material(const MItem& it) {
        Mat mt;
        for (const MItem& k : it.kids) {
            line_ = k.line;
            if (is(k, "Shader")) mt.shader = arg(k, 1);
            if (!is(k, "Layer")) continue;
            Layer l;
            for (const MItem& a : k.kids) {
                line_ = a.line;
                if (is(a, "FilterMode")) l.filter = filterMode(arg(a, 1));
                else if (is(a, "Unshaded")) l.shading |= 1;
                else if (is(a, "TwoSided")) l.shading |= 0x10;
                else if (is(a, "static") && arg(a, 1) == "TextureID") l.texture = inum(arg(a, 2));
                else if (is(a, "TextureID")) { const Track t = track(a, 1); if (!t.values.empty()) l.texture = int32_t(t.values[0]); }
                else if (is(a, "static") && arg(a, 1) == "Alpha") l.alpha = num(arg(a, 2));
            }
            mt.layers.push_back(std::move(l));
        }
        m_.materials.push_back(std::move(mt));
    }

    void geoset(const MItem& it) {
        Geoset g;
        for (const MItem& k : it.kids) {
            line_ = k.line;
            auto vecs = [&](int comps, auto&& add) {
                for (const MItem& v : k.kids) {
                    std::vector<float> f = nums(v, 0);
                    if (f.size() < size_t(comps)) fail("vector needs " + std::to_string(comps) + " values");
                    add(f);
                }
            };
            if (is(k, "Vertices")) vecs(3, [&](const std::vector<float>& f) { g.pos.push_back({f[0], f[1], f[2]}); });
            else if (is(k, "Normals")) vecs(3, [&](const std::vector<float>& f) { g.nrm.push_back({f[0], f[1], f[2]}); });
            else if (is(k, "TVertices")) { if (g.uv.empty()) vecs(2, [&](const std::vector<float>& f) { g.uv.push_back({f[0], f[1]}); }); }
            else if (is(k, "Tangents")) vecs(4, [&](const std::vector<float>& f) { g.tangents.push_back({f[0], f[1], f[2], f[3]}); });
            else if (is(k, "VertexGroup")) { for (float f : nums(k)) g.vgroup.push_back(uint32_t(std::max(0.0f, f))); }
            else if (is(k, "SkinWeights")) { for (float f : nums(k, 2)) g.skin.push_back(uint8_t(std::clamp(f, 0.0f, 255.0f))); }
            else if (is(k, "Faces")) {
                for (const MItem& f : k.kids) {
                    std::vector<float> v = nums(f, 1);
                    std::vector<uint16_t> idx;
                    for (float x : v) { if (x < 0 || x > 65535) fail("face index out of range"); idx.push_back(uint16_t(x)); }
                    const uint32_t type = is(f, "Triangles") ? 4 : is(f, "TriangleStrip") ? 5 : is(f, "TriangleFan") ? 6 : 0;
                    appendFaces(g, type, idx.data(), idx.size(), m_.faceTypesSkipped);
                }
            } else if (is(k, "Groups")) {
                for (const MItem& mg : k.kids) {
                    if (!is(mg, "Matrices")) continue;
                    const std::vector<float> v = nums(mg);
                    g.groupSizes.push_back(uint32_t(v.size()));
                    for (float x : v) g.groupIndices.push_back(uint32_t(std::max(0.0f, x)));
                }
            } else if (is(k, "MaterialID")) g.material = inum(arg(k, 1));
            else if (is(k, "LevelOfDetail")) g.lod = inum(arg(k, 1));
        }
        m_.geosets.push_back(std::move(g));
    }

    std::string path_;
    const std::string& s_;
    size_t p_ = 0;
    int line_ = 1;
    Tok tok_;
    Mdlx m_;
};

} // namespace

Mdlx parseMdl(const std::string& path, const std::string& text) { return MdlReader(path, text).parse(); }

} // namespace rts::render::mdx
