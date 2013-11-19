/*
    Support functions for the Http unit tests
 */

const HOST = App.config.uris.http || "127.0.0.1:4100"

dump(App.config)

var command = Cmd.locate("http") + " --host " + HOST + " "
if (test.verbose > 2) {
    command += "-v "
}

function run(args): String {
    if (test.verbose > 1) {
        test.logTag("[TestRun]", command + args)
    }
    let result = sh(command + args)
    return result.trim()
}
