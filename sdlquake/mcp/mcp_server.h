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

// Register the mcp_http_port cvar with the engine. Call after Host_Init.
void MCP_RegisterCvars(void);

// Override the mcp_http_port cvar's default before registration; used by the
// startup `--mcp-http <port>` flag so flag and runtime-cvar paths converge.
void MCP_SetHttpPortDefault(int port);

#endif
