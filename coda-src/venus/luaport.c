#include <lua.h>
#include <lauxlib.h>

#define BUFLEN 65536
struct socket {
    int fd;

    char buffer[BUFLEN];
    off_t offset;
    size_t length;

    struct dllist readers;
    struct dllist writers;
};

static int l_sleep(lua_State *L)
{
    struct socket *sock;
    double timeout;
    struct timeval tv;

    sock = luaL_checkudata(L, 1, SOCKFD);
    timeout = luaL_checknumber(L, 2);

    gettimeofday(&tv, NULL);
    tv.tv_sec += timeout;
    tv.tv_usec += (timeout - (double)((int)timeout)) * 1000000;
    if (tv.tv_usec >= 1000000) tv.tv_sec++;

    queue_sleep(sock, L, &tv);
    return lua_yield(L, 0);
}

static int l_write(lua_State *L)
{
    struct socket *sock;
    const char *buf;
    size_t len;
    ssize_t n = 0;

    sock = luaL_checkudata(L, 1, SOCKFD);
    buf = luaL_checklstring(L, 2, &len);

    if (is_empty(sock->write_queue) && (n = write(sockfd, buf, len)) == -1)
	return luaL_error(L, "write failed: %s", strerror(errno));

    if (n == len)
	return 0;

    queue_write(sock, L, n);
    return lua_yield(L, 0);
}

function write(data)
    local q, n = writequeue, 0
    if q.head > q.tail then
	n = socket_write(data)
    end
    if n < #data then
	q.tail = q.tail+1 
	q[q.tail] = { buffer=data, offset=n }
    end
end

void thread_main(void)
{
    while(1) {
	select(fdset, timeout);

}
/* dequeue
    while (len) {
	n = write(sockfd, buf, len);
	if (n == -1)
	    return luaL_error(L, "write failed: %s", strerror(errno));

	len -= n;
    }
    return 0;
}
*/

void MarinerLog(const char *fmt, ...)
{
    va_list ap;
    lua_State *L = mariner_state;
    char msg[BUFSIZ];
    int len;

    va_start(ap, fmt);
    len = vsnprintf(msg, BUFSIZ, fmt, ap);
    va_end(ap);

    if (len >= BUFSIZ) {
	strcpy(&msg[BUFSIZ-4], "...");
	len = BUFSIZ-1;
    }

    lua_getglobal(L, "log");
    lua_pushlstring(L, msg, len+1);
    if (lua_pcall(L, 1, 0, 0)) {
	eprint("mariner.log error: %s", lua_tostring(L, -1));
	lua_pop(L, 1);
    }
}

void MarinerReport(VenusFid *fid, uid_t uid)
{
    lua_State *L = mariner_state;
    char path[BUFSIZ];
    vproc *vp = VProcSelf();

    vp->GetPath(fid, path, BUFSIZ, 1);

    lua_getglobal(L, "report");
    lua_pushstring(L, path);
    lua_pushinteger(L, uid);
    if (lua_pcall(L, 2, 0, 0)) {
	eprint("mariner.report error: %s", lua_tostring(L, -1));
	lua_pop(L, 1);
    }
}

#if 0
static void coda_newfid(lua_State *L, VenusFid *fid)
{
    VenusFid *lua_fid = lua_newuserdata(L, sizeof(VenusFid));
    *lua_fid = *fid;

    luaL_newmetatable(L, "VenusFid");
    lua_setmetatable(L, -2);
}

static int l_getpath(lua_State *L)
{
    VenusFid *fid = luaL_checkudata(L, 1, "VenusFid");
    vproc *vp = VProcSelf();
    vp->GetPath(fid, path, BUFSIZ, 1);
    lua_pushstring(L, path);
    return 1;
}
#endif
