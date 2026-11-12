using System;
using System.Runtime.InteropServices;

namespace WonderfulMalang
{
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct NativeModelFileHeader
    {
        public uint FormatVersion;
        public uint ImporterVersion;
        public uint ChunkCount;
        public ulong SourceHash;
    }

    public static class NativeBridge
    {
        private const string HostLibrary = "NamaDllHostCppAnda.dll";

        [DllImport(HostLibrary, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.I4)]
        public static extern int NativeBridge_SetModelFileHeader(ref NativeModelFileHeader header);

        [DllImport(HostLibrary, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.I4)]
        public static extern int NativeBridge_GetModelFileHeader(out NativeModelFileHeader header);

        [DllImport(HostLibrary, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.I4)]
        public static extern int NativeBridge_SetChunkData(IntPtr data, uint size);

        [DllImport(HostLibrary, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern IntPtr NativeBridge_GetChunkData(out uint size);

        [DllImport(HostLibrary, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern void NativeBridge_ClearChunkData();

        public static int SubmitChunkData(byte[] data)
        {
            if (data == null)
                throw new ArgumentNullException(nameof(data));
            if (data.Length == 0)
                return 0;

            GCHandle pin = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                return NativeBridge_SetChunkData(pin.AddrOfPinnedObject(), checked((uint)data.Length));
            }
            finally
            {
                pin.Free();
            }
        }
    }
}
