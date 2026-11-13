// Wavefront OBJ/MTL export of the visible meshes (for checking in other tools).
#include "internal.h"

#include <windows.h>

#include <cctype>
#include <cstdio>

namespace ma2m {

std::string TextureSlug(const std::string& texture_path) {
    std::string p = NormalizePath(texture_path);
    if (detail::StartsWithI(p, "texture_dump/")) p = p.substr(13);
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos) p.resize(dot);
    for (char& c : p)
        if (!isalnum((unsigned char)c) && c != '-') c = '_';
    return p.empty() ? std::string("texture") : p;
}

namespace {
FILE* OpenUtf8(const std::string& path, const wchar_t* mode) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), mode);
}
}  // namespace

bool WriteObj(const Model& model, const std::string& obj_path, const std::string& tex_dir, std::string* error) {
    std::string mtl_path = obj_path;
    size_t dot = mtl_path.find_last_of('.');
    size_t slash = mtl_path.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) mtl_path.resize(dot);
    mtl_path += ".mtl";
    FILE* f = OpenUtf8(obj_path, L"wb");
    FILE* m = OpenUtf8(mtl_path, L"wb");
    if (!f || !m) {
        if (f) fclose(f);
        if (m) fclose(m);
        if (error) *error = "cannot write " + obj_path;
        return false;
    }
    fprintf(f, "# %s (ma2models; Z negated: game space is left-handed)\nmtllib %s\n", model.path.c_str(),
            detail::FileName(mtl_path).c_str());
    for (size_t i = 0; i < model.materials.size(); i++) {
        const Material& mat = model.materials[i];
        fprintf(m, "newmtl m%zu\n# %s (%s, %s)\nKd %.3f %.3f %.3f\nKe %.3f %.3f %.3f\nd %.3f\n", i, mat.name.c_str(),
                mat.type.c_str(), AlphaModeName(mat.alpha), mat.diffuse[0], mat.diffuse[1], mat.diffuse[2],
                mat.selfillum[0], mat.selfillum[1], mat.selfillum[2], mat.opacity);
        if (!mat.diffuse_texture.empty()) {
            std::string t = (tex_dir.empty() ? std::string() : tex_dir + "/") + TextureSlug(mat.diffuse_texture) + ".png";
            fprintf(m, "map_Kd %s\n", t.c_str());
            if (mat.alpha == AlphaMode::Cutout || mat.alpha == AlphaMode::Blend) fprintf(m, "map_d %s\n", t.c_str());
        }
        fprintf(m, "\n");
    }
    fprintf(m, "newmtl none\nKd 0.7 0.7 0.7\n");
    size_t vbase = 1, tbase = 1, nbase = 1;
    std::vector<float> pos, nrm;
    for (const auto& sm : model.meshes) {
        if (!sm.visible) continue;
        BakeSubMesh(sm, pos, nrm);
        size_t n = pos.size() / 3;
        fprintf(f, "o %s\n", sm.name.empty() ? "mesh" : sm.name.c_str());
        for (size_t v = 0; v < n; v++) fprintf(f, "v %.5f %.5f %.5f\n", pos[v * 3], pos[v * 3 + 1], -pos[v * 3 + 2]);
        bool uv = sm.uv0.size() == n * 2, hn = nrm.size() == n * 3;
        if (uv)
            for (size_t v = 0; v < n; v++) fprintf(f, "vt %.5f %.5f\n", sm.uv0[v * 2], 1.0f - sm.uv0[v * 2 + 1]);
        if (hn)
            for (size_t v = 0; v < n; v++) fprintf(f, "vn %.4f %.4f %.4f\n", nrm[v * 3], nrm[v * 3 + 1], -nrm[v * 3 + 2]);
        if (sm.material >= 0) fprintf(f, "usemtl m%d\n", sm.material);
        else fprintf(f, "usemtl none\n");
        for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
            // Mirroring Z reverses winding; swap two corners to keep fronts facing out.
            size_t i0 = sm.indices[t], i1 = sm.indices[t + 2], i2 = sm.indices[t + 1];
            size_t a = i0 + vbase, b = i1 + vbase, c = i2 + vbase;
            size_t ta = i0 + tbase, tb = i1 + tbase, tc = i2 + tbase;
            size_t na = i0 + nbase, nb = i1 + nbase, nc = i2 + nbase;
            if (uv && hn) fprintf(f, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, ta, na, b, tb, nb, c, tc, nc);
            else if (uv) fprintf(f, "f %zu/%zu %zu/%zu %zu/%zu\n", a, ta, b, tb, c, tc);
            else if (hn) fprintf(f, "f %zu//%zu %zu//%zu %zu//%zu\n", a, na, b, nb, c, nc);
            else fprintf(f, "f %zu %zu %zu\n", a, b, c);
        }
        vbase += n;
        if (uv) tbase += n;
        if (hn) nbase += n;
    }
    fclose(f);
    fclose(m);
    return true;
}

}  // namespace ma2m
