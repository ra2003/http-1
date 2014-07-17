/*
    Support functions for the Http unit tests
 */

module support {
    const HOST = App.getenv('TE_HTTP_URI') || "127.0.0.1:4100"

    var httpcmd = Cmd.locate("http") + " --host " + HOST + " "
    /*
    if (test.verbose > 2) {
        httpcmd += "-v "
    }
    */

    function http(args): String {
        let result = Cmd.run(httpcmd + args)
        return result.trim()
    }

    /*

    let http = tget('http')

    Cmd.run(http + '
    */
}
