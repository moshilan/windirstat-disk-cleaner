# Test-only launcher. Runs the synthetic test process with an ordinary primary
# token even when the hosted Runner's PowerShell process is elevated.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [string[]]$Arguments = @(),
    [string]$WorkingDirectory = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathFullyQualified($Executable) -or -not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw 'An existing absolute test executable is required.'
}
if (-not [IO.Path]::IsPathFullyQualified($WorkingDirectory) -or -not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
    throw 'An existing absolute test working directory is required.'
}
if (-not ('LimitedSyntheticTest' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class LimitedSyntheticTest
{
    [StructLayout(LayoutKind.Sequential)] struct SID_AND_ATTRIBUTES { public IntPtr Sid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential)] struct TOKEN_MANDATORY_LABEL { public SID_AND_ATTRIBUTES Label; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct STARTUPINFO {
        public uint cb; public string reserved, desktop, title;
        public uint x,y,xSize,ySize,xChars,yChars,fill,flags;
        public ushort show,reserved2; public IntPtr reservedBytes,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr process,thread; public uint pid,tid; }
    [StructLayout(LayoutKind.Sequential)] struct SECURITY_ATTRIBUTES { public int size; public IntPtr descriptor; [MarshalAs(UnmanagedType.Bool)] public bool inherit; }
    public sealed class Result { public int ExitCode; public string Output; }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CreatePipe(out IntPtr read,out IntPtr write,ref SECURITY_ATTRIBUTES security,uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetHandleInformation(IntPtr handle,uint mask,uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFile(string path,uint access,uint share,ref SECURITY_ATTRIBUTES security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateProcess(IntPtr process,uint code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle,uint timeout);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr handle,out uint code);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int info,IntPtr data,uint size,out uint needed);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool CreateRestrictedToken(IntPtr token,uint flags,uint disabledCount,ref SID_AND_ATTRIBUTES disabled,uint privilegeCount,IntPtr privileges,uint restrictedCount,IntPtr restricted,out IntPtr result);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool ConvertStringSidToSid(string sid,out IntPtr result);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool SetTokenInformation(IntPtr token,int info,ref TOKEN_MANDATORY_LABEL data,uint size);
    [DllImport("advapi32.dll")] static extern uint GetLengthSid(IntPtr sid);
    [DllImport("advapi32.dll")] static extern bool EqualSid(IntPtr first,IntPtr second);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr value);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcessAsUser(IntPtr token,string app,StringBuilder command,IntPtr processSecurity,IntPtr threadSecurity,bool inherit,uint flags,IntPtr environment,string cwd,ref STARTUPINFO startup,out PROCESS_INFORMATION process);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app,StringBuilder command,IntPtr processSecurity,IntPtr threadSecurity,bool inherit,uint flags,IntPtr environment,string cwd,ref STARTUPINFO startup,out PROCESS_INFORMATION process);

    static void Require(bool value,string action) { if(!value) throw new Win32Exception(Marshal.GetLastWin32Error(),action); }
    static bool Elevated(IntPtr token) {
        IntPtr data=Marshal.AllocHGlobal(4);
        try { uint size; Require(GetTokenInformation(token,20,data,4,out size),"Read token elevation"); return Marshal.ReadInt32(data)!=0; }
        finally { Marshal.FreeHGlobal(data); }
    }
    static bool AdminEnabled(IntPtr token,IntPtr admin) {
        uint size; GetTokenInformation(token,2,IntPtr.Zero,0,out size);
        if(size==0) throw new InvalidOperationException("Missing token groups.");
        IntPtr data=Marshal.AllocHGlobal((int)size);
        try {
            Require(GetTokenInformation(token,2,data,size,out size),"Read token groups");
            int count=Marshal.ReadInt32(data), offset=IntPtr.Size==8?8:4, stride=Marshal.SizeOf(typeof(SID_AND_ATTRIBUTES));
            for(int i=0;i<count;i++) {
                SID_AND_ATTRIBUTES group=(SID_AND_ATTRIBUTES)Marshal.PtrToStructure(IntPtr.Add(data,offset+i*stride),typeof(SID_AND_ATTRIBUTES));
                if(EqualSid(group.Sid,admin) && (group.Attributes&4)!=0 && (group.Attributes&16)==0) return true;
            }
            return false;
        } finally { Marshal.FreeHGlobal(data); }
    }
    static string Quote(string value) {
        if(value==null) throw new ArgumentNullException("argument");
        StringBuilder result=new StringBuilder("\""); int slashes=0;
        foreach(char ch in value) {
            if(ch=='\\') { slashes++; continue; }
            if(ch=='\"') result.Append('\\',slashes*2+1); else result.Append('\\',slashes);
            result.Append(ch); slashes=0;
        }
        result.Append('\\',slashes*2); result.Append('"'); return result.ToString();
    }
    public static Result Run(string executable,string[] arguments,string cwd) {
        IntPtr current=IntPtr.Zero,restricted=IntPtr.Zero,admin=IntPtr.Zero,medium=IntPtr.Zero,read=IntPtr.Zero,write=IntPtr.Zero,input=IntPtr.Zero,childToken=IntPtr.Zero;
        PROCESS_INFORMATION process=new PROCESS_INFORMATION();
        bool completed=false;
        try {
            Require(OpenProcessToken(GetCurrentProcess(),0x02000000,out current),"Open own test token");
            Require(ConvertStringSidToSid("S-1-5-32-544",out admin),"Create Administrators SID");
            IntPtr token=current;
            if(Elevated(current)||AdminEnabled(current,admin)) {
                SID_AND_ATTRIBUTES disabled=new SID_AND_ATTRIBUTES { Sid=admin,Attributes=0 };
                Require(CreateRestrictedToken(current,5,1,ref disabled,0,IntPtr.Zero,0,IntPtr.Zero,out restricted),"Create ordinary test token");
                token=restricted;
                Require(ConvertStringSidToSid("S-1-16-8192",out medium),"Create medium integrity SID");
                TOKEN_MANDATORY_LABEL label=new TOKEN_MANDATORY_LABEL { Label=new SID_AND_ATTRIBUTES { Sid=medium,Attributes=0x20 } };
                Require(SetTokenInformation(token,25,ref label,(uint)Marshal.SizeOf(typeof(TOKEN_MANDATORY_LABEL))+GetLengthSid(medium)),"Set test token medium integrity");
            }
            if(Elevated(token)||AdminEnabled(token,admin)) throw new InvalidOperationException("Refusing an elevated synthetic test process.");
            StringBuilder command=new StringBuilder(Quote(executable));
            foreach(string argument in arguments) command.Append(' ').Append(Quote(argument));
            SECURITY_ATTRIBUTES security=new SECURITY_ATTRIBUTES { size=Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES)),inherit=true };
            Require(CreatePipe(out read,out write,ref security,0),"Create test output pipe");
            Require(SetHandleInformation(read,1,0),"Keep test output reader private");
            input=CreateFile("NUL",0x80000000,3,ref security,3,0,IntPtr.Zero);
            if(input==new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error(),"Open test null input");
            STARTUPINFO startup=new STARTUPINFO { cb=(uint)Marshal.SizeOf(typeof(STARTUPINFO)),flags=0x100,input=input,output=write,error=write };
            bool created=restricted!=IntPtr.Zero?
                CreateProcessAsUser(token,executable,command,IntPtr.Zero,IntPtr.Zero,true,4,IntPtr.Zero,cwd,ref startup,out process):
                CreateProcess(executable,command,IntPtr.Zero,IntPtr.Zero,true,4,IntPtr.Zero,cwd,ref startup,out process);
            Require(created,"Start ordinary synthetic test process suspended");
            Require(OpenProcessToken(process.process,8,out childToken),"Read actual child token before resume");
            if(Elevated(childToken)||AdminEnabled(childToken,admin)) throw new InvalidOperationException("Actual child token still has administrator authority.");
            if(ResumeThread(process.thread)==0xFFFFFFFF) throw new Win32Exception(Marshal.GetLastWin32Error(),"Resume ordinary test process");
            CloseHandle(write); write=IntPtr.Zero;
            string output;
            using(FileStream stream=new FileStream(new SafeFileHandle(read,true),FileAccess.Read)) {
                read=IntPtr.Zero;
                using(StreamReader reader=new StreamReader(stream,Encoding.UTF8,true)) output=reader.ReadToEnd();
            }
            if(WaitForSingleObject(process.process,0xFFFFFFFF)!=0) throw new Win32Exception(Marshal.GetLastWin32Error(),"Wait for synthetic test process");
            uint code; Require(GetExitCodeProcess(process.process,out code),"Read synthetic test exit code");
            completed=true; return new Result { ExitCode=unchecked((int)code),Output=output };
        } finally {
            if(!completed&&process.process!=IntPtr.Zero) TerminateProcess(process.process,1);
            if(process.thread!=IntPtr.Zero) CloseHandle(process.thread);
            if(process.process!=IntPtr.Zero) CloseHandle(process.process);
            if(childToken!=IntPtr.Zero) CloseHandle(childToken);
            if(read!=IntPtr.Zero) CloseHandle(read);
            if(write!=IntPtr.Zero) CloseHandle(write);
            if(input!=IntPtr.Zero&&input!=new IntPtr(-1)) CloseHandle(input);
            if(restricted!=IntPtr.Zero) CloseHandle(restricted);
            if(current!=IntPtr.Zero) CloseHandle(current);
            if(admin!=IntPtr.Zero) LocalFree(admin);
            if(medium!=IntPtr.Zero) LocalFree(medium);
        }
    }
}
'@
}
$result = [LimitedSyntheticTest]::Run($Executable, $Arguments, $WorkingDirectory)
$global:LASTEXITCODE = $result.ExitCode
if ($result.Output) { Write-Output $result.Output.TrimEnd() }
if ($result.ExitCode -ne 0) { throw "Limited synthetic test process failed with exit code $($result.ExitCode)" }
