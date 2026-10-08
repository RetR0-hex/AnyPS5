import ctypes as ct
from ctypes import wintypes as wt
import os
from pathlib import Path
import struct
import zlib


class ThreadEntry(ct.Structure):
    _fields_ = [('size', wt.DWORD), ('usage', wt.DWORD), ('tid', wt.DWORD),
                ('pid', wt.DWORD), ('priority', wt.LONG), ('delta', wt.LONG), ('flags', wt.DWORD)]


class ModuleEntry(ct.Structure):
    _fields_ = [('size', wt.DWORD), ('id', wt.DWORD), ('pid', wt.DWORD),
                ('global_count', wt.DWORD), ('process_count', wt.DWORD), ('base', ct.c_void_p),
                ('bytes', wt.DWORD), ('handle', wt.HMODULE), ('name', wt.WCHAR * 256),
                ('path', wt.WCHAR * 260)]


def kernel_api():
    if os.name != 'nt':
        raise OSError('Thread snapshots require Windows')
    kernel = ct.WinDLL('kernel32', use_last_error=True)
    signatures = {
        'CreateToolhelp32Snapshot': ([wt.DWORD, wt.DWORD], wt.HANDLE),
        'Module32FirstW': ([wt.HANDLE, ct.POINTER(ModuleEntry)], wt.BOOL),
        'Module32NextW': ([wt.HANDLE, ct.POINTER(ModuleEntry)], wt.BOOL),
        'Thread32First': ([wt.HANDLE, ct.POINTER(ThreadEntry)], wt.BOOL),
        'Thread32Next': ([wt.HANDLE, ct.POINTER(ThreadEntry)], wt.BOOL),
        'OpenProcess': ([wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
        'OpenThread': ([wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
        'SuspendThread': ([wt.HANDLE], wt.DWORD),
        'ResumeThread': ([wt.HANDLE], wt.DWORD),
        'GetThreadContext': ([wt.HANDLE, ct.c_void_p], wt.BOOL),
        'ReadProcessMemory': ([wt.HANDLE, ct.c_void_p, ct.c_void_p, ct.c_size_t,
                               ct.POINTER(ct.c_size_t)], wt.BOOL),
        'CloseHandle': ([wt.HANDLE], wt.BOOL),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes, function.restype = arguments, result
    return kernel


def guest_section(path):
    try:
        with open(path, 'rb') as stream:
            header = stream.read(64)
            if header[:2] != b'MZ':
                return None
            stream.seek(struct.unpack_from('<I', header, 60)[0])
            header = stream.read(24)
            if header[:4] != b'PE\0\0':
                return None
            count, = struct.unpack_from('<H', header, 6)
            optional_size, = struct.unpack_from('<H', header, 20)
            stream.seek(optional_size, 1)
            for _ in range(count):
                section = stream.read(40)
                if section[:8].rstrip(b'\0') == b'.elf0':
                    return struct.unpack_from('<I', section, 12)[0]
    except (OSError, struct.error):
        pass
    return None


def snapshot_threads(pid):
    kernel = kernel_api()
    modules = []
    snapshot = kernel.CreateToolhelp32Snapshot(0x18, pid)
    if snapshot == ct.c_void_p(-1).value:
        raise ct.WinError(ct.get_last_error())
    try:
        entry = ModuleEntry()
        entry.size = ct.sizeof(entry)
        present = kernel.Module32FirstW(snapshot, ct.byref(entry))
        while present:
            modules.append({'name': entry.name, 'path': entry.path, 'base': entry.base,
                            'size': entry.bytes, 'guest_section_rva': guest_section(entry.path)})
            present = kernel.Module32NextW(snapshot, ct.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)

    def describe(address):
        for module in modules:
            offset = address - module['base']
            if 0 <= offset < module['size']:
                result = f"{module['name']}+0x{offset:x}"
                guest = module['guest_section_rva']
                if guest is not None and offset >= guest:
                    result += f' (guest vaddr 0x{offset - guest:x})'
                return result
        return f'0x{address:x}'

    def guest_address(address):
        return any(module['base'] <= address < module['base'] + module['size']
                   and (module['guest_section_rva'] is not None or '.prx' in module['name'])
                   for module in modules)

    process = kernel.OpenProcess(0x410, False, pid)
    if not process:
        raise ct.WinError(ct.get_last_error())
    output = [f'Process {pid}', 'Modules:']
    output.extend(f"  {module['name']}: 0x{module['base']:x}+0x{module['size']:x} {module['path']}"
                  for module in modules)
    try:
        def read(address, length):
            data = ct.create_string_buffer(length)
            size = ct.c_size_t()
            kernel.ReadProcessMemory(process, address, data, length, ct.byref(size))
            return data.raw[:size.value]

        thread_ids = []
        snapshot = kernel.CreateToolhelp32Snapshot(4, 0)
        if snapshot == ct.c_void_p(-1).value:
            raise ct.WinError(ct.get_last_error())
        try:
            entry = ThreadEntry()
            entry.size = ct.sizeof(entry)
            present = kernel.Thread32First(snapshot, ct.byref(entry))
            while present:
                if entry.pid == pid:
                    thread_ids.append(entry.tid)
                present = kernel.Thread32Next(snapshot, ct.byref(entry))
        finally:
            kernel.CloseHandle(snapshot)

        for tid in thread_ids:
            handle = kernel.OpenThread(0x4a, False, tid)
            if not handle:
                output.append(f'Thread {tid}: unavailable, Windows error {ct.get_last_error()}')
                continue
            suspended = False
            try:
                if kernel.SuspendThread(handle) == 0xffffffff:
                    continue
                suspended = True
                raw = ct.create_string_buffer(1250)
                address = (ct.addressof(raw) + 15) & ~15
                ct.c_uint32.from_address(address + 48).value = 0x100003
                if not kernel.GetThreadContext(handle, address):
                    output.append(f'Thread {tid}: context unavailable, Windows error {ct.get_last_error()}')
                    continue
                context = ct.string_at(address, 1232)
                registers = struct.unpack_from('<17Q', context, 120)
                rsp, rbp, rip = registers[4], registers[5], registers[16]
                output.append(f'Thread {tid}: {describe(rip)} rsp=0x{rsp:x} rbp=0x{rbp:x}')
                names = 'rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15 rip'.split()
                output.append('  registers: ' + ' '.join(f'{name}={value:016x}' for name, value in zip(names, registers)))
                frames = []
                for _ in range(16):
                    pair = read(rbp, 16)
                    if len(pair) != 16:
                        break
                    following, returned = struct.unpack('<QQ', pair)
                    frames.append(describe(returned))
                    if not rbp < following < rbp + 0x100000:
                        break
                    rbp = following
                if frames:
                    output.append('  frame pointers: ' + ' -> '.join(frames))
                stack, candidates = read(rsp, 4096), []
                for offset in range(0, len(stack) - 7, 8):
                    value, = struct.unpack_from('<Q', stack, offset)
                    if guest_address(value):
                        item = f'[rsp+0x{offset:x}] {describe(value)}'
                        candidates.append(item)
                    if len(candidates) >= 24:
                        break
                if candidates:
                    output.append('  stack candidates: ' + ' -> '.join(candidates))
            finally:
                if suspended:
                    kernel.ResumeThread(handle)
                kernel.CloseHandle(handle)
    finally:
        kernel.CloseHandle(process)
    return '\n'.join(output) + '\n', modules


def capture_window(pid, output):
    if os.name != 'nt':
        return False
    user = ct.WinDLL('user32', use_last_error=True)
    gdi = ct.WinDLL('gdi32', use_last_error=True)
    callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    windows = []
    user.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
    user.IsWindowVisible.argtypes = [wt.HWND]
    user.EnumWindows.argtypes = [callback_type, wt.LPARAM]
    user.GetClientRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
    user.GetDC.argtypes, user.GetDC.restype = [wt.HWND], wt.HDC
    user.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
    user.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
    gdi.CreateCompatibleDC.argtypes, gdi.CreateCompatibleDC.restype = [wt.HDC], wt.HDC
    gdi.CreateCompatibleBitmap.argtypes, gdi.CreateCompatibleBitmap.restype = [wt.HDC, ct.c_int, ct.c_int], wt.HBITMAP
    gdi.SelectObject.argtypes, gdi.SelectObject.restype = [wt.HDC, wt.HGDIOBJ], wt.HGDIOBJ
    gdi.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ct.c_void_p, ct.c_void_p, wt.UINT]
    gdi.DeleteObject.argtypes = [wt.HGDIOBJ]
    gdi.DeleteDC.argtypes = [wt.HDC]

    @callback_type
    def visit(window, _):
        owner = wt.DWORD()
        user.GetWindowThreadProcessId(window, ct.byref(owner))
        if owner.value == pid and user.IsWindowVisible(window):
            rect = wt.RECT()
            if user.GetClientRect(window, ct.byref(rect)) and rect.right > 0 and rect.bottom > 0:
                windows.append((rect.right * rect.bottom, window, rect.right, rect.bottom))
        return True

    user.EnumWindows(visit, 0)
    if not windows:
        return False
    _, window, width, height = max(windows)
    source = user.GetDC(window)
    if not source:
        raise ct.WinError(ct.get_last_error())
    destination = bitmap = previous = None
    try:
        destination = gdi.CreateCompatibleDC(source)
        bitmap = gdi.CreateCompatibleBitmap(source, width, height)
        if not destination or not bitmap:
            raise ct.WinError(ct.get_last_error())
        previous = gdi.SelectObject(destination, bitmap)
        if not user.PrintWindow(window, destination, 3):
            return False
        gdi.SelectObject(destination, previous)
        previous = None
        header = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
        header = ct.create_string_buffer(header)
        pixels = ct.create_string_buffer(width * height * 4)
        if gdi.GetDIBits(destination, bitmap, 0, height, pixels, header, 0) != height:
            return False
        rows = bytearray()
        data = pixels.raw
        for y in range(height):
            rows.append(0)
            row = data[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
            rows.extend(rgb)

        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
        Path(output).write_bytes(png)
        return True
    finally:
        if previous:
            gdi.SelectObject(destination, previous)
        if bitmap:
            gdi.DeleteObject(bitmap)
        if destination:
            gdi.DeleteDC(destination)
        user.ReleaseDC(window, source)
