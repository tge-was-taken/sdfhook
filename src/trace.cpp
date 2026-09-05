#include "trace.h"

namespace sdfhook {
namespace {

// Local because the runner exposes only a C API, so its own writer is out of reach.
class JsonRow {
public:
    JsonRow() { mText = "{"; }

    JsonRow& add(const char* key, u64 value) {
        return field(key) += std::to_string(value), *this;
    }

    JsonRow& add(const char* key, s64 value) {
        return field(key) += std::to_string(value), *this;
    }

    JsonRow& add(const char* key, f32 value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", double(value));
        return field(key) += buf, *this;
    }

    JsonRow& add(const char* key, bool value) {
        return field(key) += value ? "true" : "false", *this;
    }

    JsonRow& add(const char* key, const char* value) {
        field(key) += '"';
        for (const char* c = value; c && *c; c++) {
            if (*c == '"' || *c == '\\') mText += '\\';
            mText += *c;
        }
        mText += '"';
        return *this;
    }

    JsonRow& addHex(const char* key, u64 value, int digits = 8) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%0*llX", digits, static_cast<unsigned long long>(value));
        return add(key, buf);
    }

    template <typename T>
    JsonRow& addHex(const char* key, guest_ptr<T> pointer, int digits = 8) {
        return addHex(key, u64(pointer.addr), digits);
    }

    std::string take() {
        mText += "}\n";
        return std::move(mText);
    }

private:
    std::string& field(const char* key) {
        if (mText.size() > 1) mText += ',';
        mText += '"';
        mText += key;
        mText += "\":";
        return mText;
    }

    std::string mText;
};

}  // namespace

Trace::Trace(pcsx2hook_ctx* runtime, const std::string& path) : mRuntime(runtime), mGuest(runtime) {
    mFile = std::fopen(path.c_str(), "w");
    if (!mFile) {
        std::fprintf(stderr, "[sdf] cannot open %s\n", path.c_str());
    }
}

Trace::~Trace() {
    if (mFile) std::fclose(mFile);
}

void Trace::write(const std::string& row) {
    if (mFile) std::fwrite(row.data(), 1, row.size(), mFile);
}

void Trace::onFrame(u32 frame) {
    mFrame = frame;
    write(JsonRow().add("ev", "frame").add("seq", mSeq++).add("frame", u64(frame)).take());
}

void Trace::onDds3DrawProcess(u32 arg) {
    write(JsonRow()
              .add("ev", "dds3DrawProcess")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("arg", arg)
              .take());
}

void Trace::onSdf3DSetCamera(u32 cameraAddr, u32 screenParamAddr) {
    write(JsonRow()
              .add("ev", "sdf3DSetCamera")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("camera", cameraAddr)
              .addHex("screenParam", screenParamAddr)
              .take());
}

void Trace::onSdfModelLinkOt(u32 otAddr, u32 rootAddr) {
    sdfModelRootNode_t root{};
    const bool ok = mGuest.read(rootAddr, root);
    sdfModelNodeList_t list{};
    if (ok) mGuest.read(root.nodeList, list);

    sdfModelOt_t ot{};
    mGuest.read(otAddr, ot);

    JsonRow row;
    row.add("ev", "sdfModelLinkOt")
        .add("seq", mSeq++)
        .add("frame", u64(mFrame))
        .addHex("ot", otAddr)
        .addHex("root", rootAddr);
    if (ok) {
        // flags 0x08 suppresses the shaped and DMA paths, and a negative colour flips the
        // default ordering table.
        row.add("flags", u64(root.flags))
            .add("drawAttr", u64(root.drawAttr))
            .addHex("color", root.color)
            .add("nodes", u64(u16(list.count)))
            .addHex("matList", root.matList)
            .addHex("light", root.light)
            .addHex("fog", root.fog)
            .add("zBias", root.zBias)
            .add("faceDivBias", root.faceDivBias);
    }
    row.addHex("ot0", ot.ot[0]).addHex("ot1", ot.ot[1]).addHex("ot2", ot.ot[2]).addHex("ot3",
                                                                                       ot.ot[3]);
    write(row.take());
}

void Trace::onSdfCorePacketGen(u32 nodeAddr, u32 packetAddr, u32 attrMask, u32 color) {
    JsonRow row;
    row.add("ev", "sdfCorePacketGen")
        .add("seq", mSeq++)
        .add("frame", u64(mFrame))
        .addHex("node", nodeAddr)
        .addHex("packet", packetAddr)
        .addHex("attrMask", attrMask, 4)
        .addHex("color", color);

    sdfModelNode_t node{};
    if (mGuest.read(nodeAddr, node)) {
        row.addHex("nodeId", node.id).add("nodeFlags", u64(node.flags));
    }

    // node->shape is null at draw time: the geometry travels in the packet, and geomFlags is what
    // the per-vertex path branches on.
    sdfCorePacket_t packet{};
    if (mGuest.read(packetAddr, packet)) {
        row.add("type", u64(packet.type))
            .add("verts", u64(packet.vertexCount))
            .add("elems", u64(packet.elemCount))
            .addHex("geomFlags", packet.geomFlags, 4)
            .addHex("attrFlags", packet.attrFlags, 4)
            .add("matIndex", u64(packet.matIndex));
    }
    write(row.take());
}

void Trace::onSdfModelLinkShapedDrawPacket(u32 nodeAddr, u32 shapeAddr) {
    write(JsonRow()
              .add("ev", "sdfModelLinkShapedDrawPacket")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("node", nodeAddr)
              .addHex("shape", shapeAddr)
              .take());
}

void Trace::onSdfSortOtAvlRegist(u32 otAddr, u32 tagAddr, f32 z) {
    sdfDrawTag_t tag{};
    const bool ok = mGuest.read(tagAddr, tag);
    JsonRow row;
    row.add("ev", "sdfSortOtAvlRegist")
        .add("seq", mSeq++)
        .add("frame", u64(mFrame))
        .addHex("ot", otAddr)
        .addHex("tag", tagAddr)
        .add("z", z);
    if (ok) {
        row.add("tagZ", tag.z)
            .addHex("head", tag.head)
            .addHex("tail", tag.tail)
            .addHex("light", tag.lightPacket)
            .addHex("fog", tag.fogPacket);
    }
    write(row.take());
}

void Trace::onSdfGrReqDraw(u32 tagAddr, u32 predrawAddr) {
    write(JsonRow()
              .add("ev", "sdfGrReqDraw")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("tag", tagAddr)
              .addHex("predraw", predrawAddr)
              .take());
}

void Trace::onSdfGrDrawFrame(u32 chainAddr) {
    write(JsonRow()
              .add("ev", "sdfGrDrawFrame")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("chain", chainAddr)
              .take());
}

void Trace::onSdfMatSet(u32 matAddr) {
    write(JsonRow()
              .add("ev", "sdfMatSet")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("mat", matAddr)
              .take());
}

void Trace::onSdfTexSetEx(u32 texAddr) {
    write(JsonRow()
              .add("ev", "sdfTexSetEx")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("tex", texAddr)
              .take());
}

void Trace::onPrimEntry(const char* name, u32 arg) {
    write(JsonRow()
              .add("ev", name)
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("arg", arg)
              .take());
}

void Trace::onPacket(const LinkedPacket& packet, const char* by, const DmaTag& tag, u64 verts) {
    write(JsonRow()
              .add("ev", "packet")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("packet", packet.addr)
              .add("by", by)
              .add("verts", verts)
              .add("qwc", u64(tag.qwc))
              .add("dmaId", u64(tag.id))
              .take());
}

void Trace::onSdfGrSetClearPacket(u32 packetAddr) {
    write(JsonRow()
              .add("ev", "sdfGrSetClearPacket")
              .add("seq", mSeq++)
              .add("frame", u64(mFrame))
              .addHex("packet", packetAddr)
              .take());
}

}  // namespace sdfhook
