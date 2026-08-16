using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Runtime.Versioning;

namespace GalaxyXR.VRCFaceTracking;

[SupportedOSPlatform("windows")]
internal sealed class FaceSharedMemoryReader : IDisposable
{
    internal const string MappingName = "CustomHeadsetOpenVR.GalaxyXR.Face.v1";
    private const int SnapshotBytes = 348;
    private const int GenerationOffset = 8;
    private const int SequenceOffset = 32;
    private const int FlagsOffset = 56;
    private const int StateOffset = 60;
    private const int ParameterCountOffset = 62;
    private const int ConfidenceCountOffset = 63;
    private const int WeightsOffset = 64;
    private const int ConfidenceOffset = 336;
    private static readonly long StaleTicks = Stopwatch.Frequency / 4;

    private MemoryMappedFile? mapping;
    private MemoryMappedViewAccessor? view;
    private readonly byte[] bytes = new byte[SnapshotBytes];
    private ulong lastSequence;
    private long lastSequenceTicks;

    internal bool Open()
    {
        Dispose();
        try
        {
            mapping = MemoryMappedFile.OpenExisting(MappingName, MemoryMappedFileRights.Read);
            view = mapping.CreateViewAccessor(0, SnapshotBytes, MemoryMappedFileAccess.Read);
            return true;
        }
        catch
        {
            Dispose();
            return false;
        }
    }

    internal bool TryRead(float[] weights, float[] confidences)
    {
        if (view is null || weights.Length < 68 || confidences.Length < 3)
            return false;

        for (var attempt = 0; attempt < 3; attempt++)
        {
            var generationBefore = view.ReadInt64(GenerationOffset);
            if (generationBefore <= 0 || (generationBefore & 1) != 0)
                continue;

            view.ReadArray(0, bytes, 0, bytes.Length);
            var generationAfter = view.ReadInt64(GenerationOffset);
            if (generationBefore != generationAfter || (generationAfter & 1) != 0)
                continue;

            if (bytes[0] != (byte)'G' || bytes[1] != (byte)'X' ||
                bytes[2] != (byte)'F' || bytes[3] != (byte)'1' ||
                BitConverter.ToUInt16(bytes, 4) != 1 ||
                BitConverter.ToUInt16(bytes, 6) != SnapshotBytes ||
                (BitConverter.ToUInt32(bytes, FlagsOffset) & 1U) == 0 ||
                bytes[StateOffset] != 2 ||
                bytes[ParameterCountOffset] != 68 ||
                bytes[ConfidenceCountOffset] != 3)
                return false;

            var sequence = BitConverter.ToUInt64(bytes, SequenceOffset);
            var now = Stopwatch.GetTimestamp();
            if (sequence != lastSequence)
            {
                lastSequence = sequence;
                lastSequenceTicks = now;
            }
            else if (lastSequenceTicks == 0 || now - lastSequenceTicks > StaleTicks)
            {
                return false;
            }

            for (var index = 0; index < 68; index++)
            {
                var value = BitConverter.ToSingle(bytes, WeightsOffset + index * sizeof(float));
                if (!float.IsFinite(value)) return false;
                weights[index] = Math.Clamp(value, 0.0f, 1.0f);
            }
            for (var index = 0; index < 3; index++)
            {
                var value = BitConverter.ToSingle(bytes, ConfidenceOffset + index * sizeof(float));
                if (!float.IsFinite(value)) return false;
                confidences[index] = Math.Clamp(value, 0.0f, 1.0f);
            }
            return true;
        }
        return false;
    }

    public void Dispose()
    {
        view?.Dispose();
        mapping?.Dispose();
        view = null;
        mapping = null;
        lastSequence = 0;
        lastSequenceTicks = 0;
    }
}
