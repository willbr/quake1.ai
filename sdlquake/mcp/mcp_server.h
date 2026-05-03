// mcp_server.h -- Model Context Protocol stdio server

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

// Set to 1 when --mcp flag is active; sys_sdl.c reads this to suppress stdout
extern int mcp_active;

void MCP_Init(void);
void MCP_Frame(void);
void MCP_Shutdown(void);

#endif
