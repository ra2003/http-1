/*
    Support functions for the Http unit tests
 */

module support {
    require ejs.testme

    function http(cmd): String {
        let HOST = tget('TE_HTTPS') || '127.0.0.1:4443'
        let protocol = App.env.TE_PROTOCOL ?  App.env.TE_PROTOCOL : 'http1' 
        let httpcmd = Cmd.locate('http') + ' --' + protocol + ' --host https://' + HOST + ' ' + cmd
        let result = Cmd.run(httpcmd, {exceptions: false})
        return result.trim()
    }
}
