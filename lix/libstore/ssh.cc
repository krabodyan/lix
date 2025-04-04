#include "lix/libutil/current-process.hh"
#include "lix/libutil/environment-variables.hh"
#include "lix/libstore/ssh.hh"
#include "lix/libutil/finally.hh"
#include "lix/libutil/logging.hh"
#include "lix/libutil/strings.hh"
#include "lix/libstore/temporary-dir.hh"

namespace nix {

SSHMaster::SSHMaster(const std::string & host, const std::optional<uint16_t> port, const std::string & keyFile, const std::string & sshPublicHostKey, bool useMaster, bool compress, int logFD)
    : host(host)
    , port(port)
    , fakeSSH(host == "localhost")
    , keyFile(keyFile)
    , sshPublicHostKey(sshPublicHostKey)
    , useMaster(useMaster && !fakeSSH)
    , compress(compress)
    , logFD(logFD)
{
    if (host == "" || host.starts_with("-"))
        throw Error("invalid SSH host name '%s'", host);

    auto state(state_.lock());
    state->tmpDir = std::make_unique<AutoDelete>(createTempDir("", "nix", true, true, 0700));
}

void SSHMaster::addCommonSSHOpts(Strings & args)
{
    auto state(state_.lock());

    if (port.has_value())
        args.insert(args.end(), {"-p", std::to_string(*port)});
    for (auto & i : tokenizeString<Strings>(getEnv("NIX_SSHOPTS").value_or("")))
        args.push_back(i);
    if (!keyFile.empty())
        args.insert(args.end(), {"-i", keyFile});
    if (!sshPublicHostKey.empty()) {
        Path fileName = (Path) *state->tmpDir + "/host-key";
        auto p = host.rfind("@");
        std::string thost = p != std::string::npos ? std::string(host, p + 1) : host;
        writeFile(fileName, thost + " " + base64Decode(sshPublicHostKey) + "\n");
        args.insert(args.end(), {"-oUserKnownHostsFile=" + fileName});
    }
    if (compress)
        args.push_back("-C");

    args.push_back("-oPermitLocalCommand=yes");
    args.push_back("-oLocalCommand=echo started");
}

bool SSHMaster::isMasterRunning() {
    Strings args = {"-O", "check", host};
    addCommonSSHOpts(args);

    auto res = runProgram(RunOptions {.program = "ssh", .args = args, .mergeStderrToStdout = true});
    return res.first == 0;
}

std::unique_ptr<SSHMaster::Connection> SSHMaster::startCommand(const std::string & command)
{
    Path socketPath = startMaster();

    Pipe in, out;
    in.create();
    out.create();

    auto conn = std::make_unique<Connection>();
    ProcessOptions options;
    options.dieWithParent = false;

    std::optional<Finally<std::function<void()>>> resumeLoggerDefer;
    if (!fakeSSH && !useMaster) {
        logger->pause();
        resumeLoggerDefer.emplace([&]() { logger->resume(); });
    }

    conn->sshPid = startProcess([&]() {
        restoreProcessContext();

        close(in.writeSide.get());
        close(out.readSide.get());

        if (dup2(in.readSide.get(), STDIN_FILENO) == -1)
            throw SysError("duping over stdin");
        if (dup2(out.writeSide.get(), STDOUT_FILENO) == -1)
            throw SysError("duping over stdout");
        if (logFD != -1 && dup2(logFD, STDERR_FILENO) == -1)
            throw SysError("duping over stderr");

        Strings args;

        if (fakeSSH) {
            args = { "bash", "-c" };
        } else {
            args = { "ssh", host.c_str(), "-x" };
            addCommonSSHOpts(args);
            if (socketPath != "")
                args.insert(args.end(), {"-S", socketPath});
        }

        args.push_back(command);
        execvp(args.begin()->c_str(), stringsToCharPtrs(args).data());

        // could not exec ssh/bash
        throw SysError("unable to execute '%s'", args.front());
    }, options);


    in.readSide.reset();
    out.writeSide.reset();

    // Wait for the SSH connection to be established,
    // So that we don't overwrite the password prompt with our progress bar.
    if (!fakeSSH && !useMaster && !isMasterRunning()) {
        std::string reply;
        try {
            reply = readLine(out.readSide.get());
        } catch (EndOfFile & e) { }

        if (reply != "started") {
            warn("SSH to '%s' failed, stdout first line: '%s'", host, reply);
            throw Error("failed to start SSH connection to '%s'", host);
        }
    }

    conn->out = std::move(out.readSide);
    conn->in = std::move(in.writeSide);

    return conn;
}

Path SSHMaster::startMaster()
{
    if (!useMaster) return "";

    auto state(state_.lock());

    if (state->sshMaster) return state->socketPath;

    state->socketPath = (Path) *state->tmpDir + "/ssh.sock";

    Pipe out;
    out.create();

    ProcessOptions options;
    options.dieWithParent = false;

    logger->pause();
    Finally cleanup = [&]() { logger->resume(); };

    if (isMasterRunning())
        return state->socketPath;

    state->sshMaster = startProcess([&]() {
        restoreProcessContext();

        close(out.readSide.get());

        if (dup2(out.writeSide.get(), STDOUT_FILENO) == -1)
            throw SysError("duping over stdout");

        Strings args = { "ssh", host.c_str(), "-M", "-N", "-S", state->socketPath };
        addCommonSSHOpts(args);
        execvp(args.begin()->c_str(), stringsToCharPtrs(args).data());

        throw SysError("unable to execute '%s'", args.front());
    }, options);

    out.writeSide.reset();

    std::string reply;
    try {
        reply = readLine(out.readSide.get());
    } catch (EndOfFile & e) { }

    if (reply != "started") {
        printTalkative("SSH master stdout first line: %s", reply);
        throw Error("failed to start SSH master connection to '%s'", host);
    }

    return state->socketPath;
}

}
