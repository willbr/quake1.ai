// mcp_server.h -- Model Context Protocol server (stdio or HTTP/SSE)

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

// Set to 1 when --mcp or --mcp-http is active; sys_sdl.c reads this to suppress stdout
extern int mcp_active;

// port == 0 → stdio transport (--mcp)
// port >  0 → HTTP/SSE transport (--mcp-http PORT), listens on localhost:PORT
void MCP_Init(int port);
void MCP_Frame(void);
void MCP_Shutdown(void);

#endif
