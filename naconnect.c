/* -*- Mode: C ; c-basic-offset: 2 -*- */
/*****************************************************************************
 *
 * ncurses ALSA MIDI sequencer patchbay
 *
 * Copyright (C) 2006 Nedko Arnaudov <nedko@arnaudov.name>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *****************************************************************************/

#include <ncurses.h>
#include <alsa/asoundlib.h>

#include "list.h"

#define MSG_OUT(format, arg...) printf(format "\n", ## arg)
#define ERR_OUT(format, arg...) fprintf(stderr, format "\n", ## arg)

struct port
{
  struct list_head siblings;
  snd_seq_port_info_t * pinfo_ptr;
};

struct list_head g_input_ports;
struct list_head g_output_ports;

void free_ports(struct list_head * ports_ptr)
{
  struct list_head * node_ptr;
  struct port * port_ptr;

  while (!list_empty(ports_ptr))
  {
    node_ptr = ports_ptr->next;

    list_del(node_ptr);

    port_ptr = list_entry(node_ptr, struct port, siblings);

    snd_seq_port_info_free(port_ptr->pinfo_ptr);
    free(port_ptr);
  }
}

void free_all_ports()
{
  free_ports(&g_input_ports);
  free_ports(&g_output_ports);
}

#define check_port_caps(pinfo_ptr, bits) ((snd_seq_port_info_get_capability(pinfo_ptr) & (bits)) == (bits))

int
add_port(const snd_seq_port_info_t * pinfo_ptr, struct list_head * ports_ptr)
{
  int ret;
  struct port * port_ptr;

  port_ptr = (struct port *)malloc(sizeof(struct port));
  if (port_ptr == NULL)
  {
    ERR_OUT("malloc() failed.");
    return -1;
  }

  ret = snd_seq_port_info_malloc(&port_ptr->pinfo_ptr);
  if (ret < 0)
  {
    ERR_OUT("Cannot allocate port info - %s", snd_strerror(ret));
    return -1;
  }

  snd_seq_port_info_copy(port_ptr->pinfo_ptr, pinfo_ptr);

  list_add_tail(&port_ptr->siblings, ports_ptr);

  return 0;
}

void build_ports(snd_seq_t * seq_handle)
{
  int ret;
  snd_seq_client_info_t * cinfo_ptr;
  snd_seq_port_info_t * pinfo_ptr;

  snd_seq_client_info_alloca(&cinfo_ptr);
  snd_seq_port_info_alloca(&pinfo_ptr);

  snd_seq_client_info_set_client(cinfo_ptr, -1);

  while (snd_seq_query_next_client(seq_handle, cinfo_ptr) >= 0)
  {
    snd_seq_port_info_set_client(pinfo_ptr, snd_seq_client_info_get_client(cinfo_ptr));
    snd_seq_port_info_set_port(pinfo_ptr, -1);
    while (snd_seq_query_next_port(seq_handle, pinfo_ptr) >= 0)
    {
      if (check_port_caps(pinfo_ptr, SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ))
      {
        ret = add_port(pinfo_ptr, &g_input_ports);
        if (ret < 0)
          goto free_ports;
      }

      if (check_port_caps(pinfo_ptr, SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE))
      {
        ret = add_port(pinfo_ptr, &g_output_ports);
        if (ret < 0)
          goto free_ports;
      }
    }
  }

  return;

free_ports:
  free_all_ports();
}

snd_seq_port_info_t *
find_port(
  unsigned int client,
  unsigned int port,
  struct list_head * ports_ptr)
{
  struct list_head * node_ptr;
  struct port * port_ptr;

  list_for_each(node_ptr, ports_ptr)
  {
    port_ptr = list_entry(node_ptr, struct port, siblings);
    if (snd_seq_port_info_get_client(port_ptr->pinfo_ptr) == client &&
        snd_seq_port_info_get_port(port_ptr->pinfo_ptr) == port)
    {
      return port_ptr->pinfo_ptr;
    }
  }

  return NULL;
}

struct connection
{
  struct list_head siblings;
  snd_seq_port_info_t * source_pinfo_ptr;
  snd_seq_port_info_t * dest_pinfo_ptr;
  unsigned int source_client;
  unsigned int source_port;
  unsigned int dest_client;
  unsigned int dest_port;
};

struct list_head g_connections;

void free_connections()
{
  struct list_head * node_ptr;
  struct connection * connection_ptr;

  while (!list_empty(&g_connections))
  {
    node_ptr = g_connections.next;

    list_del(node_ptr);

    connection_ptr = list_entry(node_ptr, struct connection, siblings);

    free(connection_ptr);
  }
}

int add_connection(
  unsigned int source_client,
  unsigned int source_port,
  unsigned int dest_client,
  unsigned int dest_port
  )
{
  struct connection * connection_ptr;

  connection_ptr = (struct connection *)malloc(sizeof(struct connection));
  if (connection_ptr == NULL)
  {
    ERR_OUT("malloc() failed.");
    return -1;
  }

  connection_ptr->source_pinfo_ptr = find_port(source_client, source_port, &g_input_ports);
  connection_ptr->dest_pinfo_ptr = find_port(dest_client, dest_port, &g_output_ports);

  connection_ptr->source_client = source_client;
  connection_ptr->source_port = source_port;
  connection_ptr->dest_client = dest_client;
  connection_ptr->dest_port = dest_port;

  list_add_tail(&connection_ptr->siblings, &g_connections);

  return 0;
}

void build_connections(snd_seq_t * seq_handle)
{
  snd_seq_client_info_t * cinfo_ptr;
  snd_seq_port_info_t * pinfo_ptr;
  snd_seq_query_subscribe_t * subscr_ptr;
  const snd_seq_addr_t * addr_ptr;

  snd_seq_client_info_alloca(&cinfo_ptr);
  snd_seq_port_info_alloca(&pinfo_ptr);
  snd_seq_query_subscribe_alloca(&subscr_ptr);

  snd_seq_client_info_set_client(cinfo_ptr, -1);

  while (snd_seq_query_next_client(seq_handle, cinfo_ptr) >= 0)
  {
    snd_seq_port_info_set_client(pinfo_ptr, snd_seq_client_info_get_client(cinfo_ptr));
    snd_seq_port_info_set_port(pinfo_ptr, -1);
    while (snd_seq_query_next_port(seq_handle, pinfo_ptr) >= 0)
    {
      snd_seq_query_subscribe_set_root(subscr_ptr, snd_seq_port_info_get_addr(pinfo_ptr));
#if 0
      snd_seq_query_subscribe_set_type(subscr_ptr, SND_SEQ_QUERY_SUBS_READ);

      snd_seq_query_subscribe_set_index(subscr_ptr, 0);
      while (snd_seq_query_port_subscribers(seq_handle, subscr_ptr) >= 0)
      {
        addr_ptr = snd_seq_query_subscribe_get_addr(subscr_ptr);

        add_connection(snd_seq_port_info_get_client(pinfo_ptr), snd_seq_port_info_get_port(pinfo_ptr), addr_ptr->client, addr_ptr->port);

        snd_seq_query_subscribe_set_index(subscr_ptr, snd_seq_query_subscribe_get_index(subscr_ptr) + 1);
      }
#endif
      snd_seq_query_subscribe_set_type(subscr_ptr, SND_SEQ_QUERY_SUBS_WRITE);

      snd_seq_query_subscribe_set_index(subscr_ptr, 0);
      while (snd_seq_query_port_subscribers(seq_handle, subscr_ptr) >= 0)
      {
        addr_ptr = snd_seq_query_subscribe_get_addr(subscr_ptr);

        add_connection(addr_ptr->client, addr_ptr->port, snd_seq_port_info_get_client(pinfo_ptr), snd_seq_port_info_get_port(pinfo_ptr));

        snd_seq_query_subscribe_set_index(subscr_ptr, snd_seq_query_subscribe_get_index(subscr_ptr) + 1);
      }
    }
  }
}

void dump_ports()
{
  struct list_head * node_ptr;
  struct port * port_ptr;

  list_for_each(node_ptr, &g_input_ports)
  {
    port_ptr = list_entry(node_ptr, struct port, siblings);

    MSG_OUT("IN: %s", snd_seq_port_info_get_name(port_ptr->pinfo_ptr));
  }

  list_for_each(node_ptr, &g_output_ports)
  {
    port_ptr = list_entry(node_ptr, struct port, siblings);

    MSG_OUT("OUT: %s", snd_seq_port_info_get_name(port_ptr->pinfo_ptr));
  }
}

struct window
{
  struct list_head * list_ptr;
  WINDOW * window_ptr;
  int selected;
  int height;
  int width;
  const char * name;
  int index;
  int count;
};

void
draw_border(struct window * window_ptr)
{
  int col;
  const char * title_prefix_ptr;
  const char * title_suffix_ptr;

  if (window_ptr->selected)
  {
    wattron(window_ptr->window_ptr, COLOR_PAIR(4));
    wattron(window_ptr->window_ptr, WA_BOLD);
  }

  /* 0, 0 gives default characters for the vertical and horizontal lines */
  box(window_ptr->window_ptr, 0, 0);

  if (window_ptr->selected)
  {
    title_prefix_ptr = "=[";
    title_suffix_ptr = "]=";
  }
  else
  {
    title_prefix_ptr = " [";
    title_suffix_ptr = "] ";
  }

  col = (window_ptr->width - strlen(window_ptr->name) - strlen(title_suffix_ptr) - strlen(title_prefix_ptr))/2;

  if (col < 0)
  {
    col = 0;
  }

  mvwprintw(
    window_ptr->window_ptr,
    0,
    col,
    title_prefix_ptr);

  mvwprintw(
    window_ptr->window_ptr,
    0,
    col+strlen(title_prefix_ptr),
    window_ptr->name);

  mvwprintw(
    window_ptr->window_ptr,
    0,
    col+strlen(title_prefix_ptr)+strlen(window_ptr->name),
    title_suffix_ptr);

  if (window_ptr->selected)
  {
    wattroff(window_ptr->window_ptr, COLOR_PAIR(4));
    wattroff(window_ptr->window_ptr, WA_BOLD);
  }
}

int
print_port_info(WINDOW * window_ptr, int row, int col, int client, int port, const char * name)
{
  char buf[1024];
  snprintf(buf, 1024, "%3u:%u %s", client, port, name);
  buf[1023] = 0;
  mvwprintw(window_ptr, row, col, "%s", buf);
  return strlen(buf);
}

void
draw_ports(struct window * window_ptr)
{
  struct list_head * node_ptr;
  struct port * port_ptr;
  int row, col;
  int rows, cols;

  getmaxyx(window_ptr->window_ptr, rows, cols);

  row = 0;

  list_for_each(node_ptr, window_ptr->list_ptr)
  {
    port_ptr = list_entry(node_ptr, struct port, siblings);

    if (row == window_ptr->index)
    {
      if (window_ptr->selected)
      {
        wattron(window_ptr->window_ptr, COLOR_PAIR(3));
      }
      else
      {
        wattron(window_ptr->window_ptr, COLOR_PAIR(2));
      }
    }
    else
    {
      wattron(window_ptr->window_ptr, COLOR_PAIR(1));
    }

    col = 1;

    col += print_port_info(
      window_ptr->window_ptr,
      row+1,
      col,
      snd_seq_port_info_get_client(port_ptr->pinfo_ptr),
      snd_seq_port_info_get_port(port_ptr->pinfo_ptr),
      snd_seq_port_info_get_name(port_ptr->pinfo_ptr));

    while (col < cols)
    {
      mvwprintw(window_ptr->window_ptr, row+1, col, " ");
      col++;
    }

    if (row == window_ptr->index)
    {
      if (window_ptr->selected)
      {
        wattroff(window_ptr->window_ptr, COLOR_PAIR(3));
      }
      else
      {
        wattroff(window_ptr->window_ptr, COLOR_PAIR(2));
      }
    }
    else
    {
      wattroff(window_ptr->window_ptr, COLOR_PAIR(1));
    }

    row++;
  }

  draw_border(window_ptr);

  wrefresh(window_ptr->window_ptr);
}

void
draw_connections(struct window * window_ptr)
{
  struct list_head * node_ptr;
  struct connection * connection_ptr;
  int row, col;
  int rows, cols;

  getmaxyx(window_ptr->window_ptr, rows, cols);

  row = 0;

  list_for_each(node_ptr, window_ptr->list_ptr)
  {
    connection_ptr = list_entry(node_ptr, struct connection, siblings);

    if (row == window_ptr->index)
    {
      if (window_ptr->selected)
      {
        wattron(window_ptr->window_ptr, COLOR_PAIR(3));
      }
      else
      {
        wattron(window_ptr->window_ptr, COLOR_PAIR(1));
      }
    }
    else
    {
      wattron(window_ptr->window_ptr, COLOR_PAIR(1));
    }

    col = 1;
    col += print_port_info(
      window_ptr->window_ptr,
      row+1,
      col,
      connection_ptr->source_client,
      connection_ptr->source_port,
      (connection_ptr->source_pinfo_ptr == NULL)?"???":snd_seq_port_info_get_name(connection_ptr->source_pinfo_ptr));

    mvwprintw(window_ptr->window_ptr, row+1, col, " ");
    col++;

    while (col < cols/2 - 3)
    {
      mvwprintw(window_ptr->window_ptr, row+1, col, "-");
      col++;
    }

    mvwprintw(window_ptr->window_ptr, row+1, col, "-");
    col++;
    mvwprintw(window_ptr->window_ptr, row+1, col, ">");
    col++;

    mvwprintw(window_ptr->window_ptr, row+1, col, " ");
    col++;

    col += print_port_info(
      window_ptr->window_ptr,
      row+1,
      col,
      connection_ptr->dest_client,
      connection_ptr->dest_port,
      (connection_ptr->dest_pinfo_ptr == NULL)?"???":snd_seq_port_info_get_name(connection_ptr->dest_pinfo_ptr));

    while (col < cols)
    {
      mvwprintw(window_ptr->window_ptr, row+1, col, " ");
      col++;
    }

    if (row == window_ptr->index)
    {
      if (window_ptr->selected)
      {
        wattroff(window_ptr->window_ptr, COLOR_PAIR(3));
      }
      else
      {
        wattroff(window_ptr->window_ptr, COLOR_PAIR(1));
      }
    }
    else
    {
      wattroff(window_ptr->window_ptr, COLOR_PAIR(1));
    }

    row++;
  }

  draw_border(window_ptr);

  wrefresh(window_ptr->window_ptr);
}

void items_count(struct window * window_ptr)
{
  struct list_head * node_ptr;

  window_ptr->count = 0;

  list_for_each(node_ptr, window_ptr->list_ptr)
  {
    window_ptr->count++;
  }

  if (window_ptr->count > 0)
  {
    if (window_ptr->index == -1)
    {
      window_ptr->index = 0;
    }
    else
    {
      if (window_ptr->index >= window_ptr->count)
      {
        window_ptr->index = window_ptr->count - 1;
      }
    }
  }
  else
  {
    window_ptr->index = -1;
  }
}

void create_ports_win(struct window * window_ptr, struct list_head * ports_ptr, int height, int width, int starty, int startx, const char * name)
{
  window_ptr->list_ptr = ports_ptr;
  window_ptr->window_ptr = newwin(height, width, starty, startx);
  window_ptr->selected = 0;
  window_ptr->width = width;
  window_ptr->height = height;
  window_ptr->name = name;
  window_ptr->index = -1;

  items_count(window_ptr);
}

void create_connections_win(struct window * window_ptr, int height, int width, int starty, int startx)
{
  window_ptr->list_ptr = &g_connections;
  window_ptr->window_ptr = newwin(height, width, starty, startx);
  window_ptr->selected = 0;
  window_ptr->width = width;
  window_ptr->height = height;
  window_ptr->name = "Connections";
  window_ptr->index = -1;

  items_count(window_ptr);
}

void ports_handle_key(struct window * window_ptr, int ch)
{
  if (ch == KEY_DOWN)
  {
    if (window_ptr->index + 1 < window_ptr->count)
    {
      window_ptr->index++;
    }

    return;
  }

  if (ch == KEY_UP)
  {
    if (window_ptr->index > 0)
    {
      window_ptr->index--;
    }

    return;
  }
}

void connections_handle_key(struct window * window_ptr, int ch)
{
  if (ch == KEY_DOWN)
  {
    if (window_ptr->index + 1 < window_ptr->count)
    {
      window_ptr->index++;
    }

    return;
  }

  if (ch == KEY_UP)
  {
    if (window_ptr->index > 0)
    {
      window_ptr->index--;
    }

    return;
  }
}

int
disconnect(snd_seq_t * seq_handle, int index)
{
  int i;
  struct list_head * node_ptr;
  struct connection * connection_ptr;
  snd_seq_port_subscribe_t * subscr_ptr;
  snd_seq_addr_t sender, dest;

  snd_seq_port_subscribe_alloca(&subscr_ptr);

  i = 0;
  list_for_each(node_ptr, &g_connections)
  {
    connection_ptr = list_entry(node_ptr, struct connection, siblings);

    if (i == index)
    {
      sender.client = connection_ptr->source_client;
      sender.port = connection_ptr->source_port;
      dest.client = connection_ptr->dest_client;
      dest.port = connection_ptr->dest_port;

      snd_seq_port_subscribe_set_sender(subscr_ptr, &sender);
      snd_seq_port_subscribe_set_dest(subscr_ptr, &dest);

      if (snd_seq_unsubscribe_port(seq_handle, subscr_ptr) < 0)
      {
        return -1;
      }

      return 0;
    }

    i++;
  }

  return -1;
}

struct port *
find_port_by_index(struct list_head * ports_ptr, int index)
{
  int i;
  struct list_head * node_ptr;

  i = 0;
  list_for_each(node_ptr, ports_ptr)
  {
    if (i == index)
    {
      return list_entry(node_ptr, struct port, siblings);
    }

    i++;
  }

  return NULL;
}

const char *
connect(snd_seq_t * seq_handle, int index_source, int index_dest)
{
  snd_seq_port_subscribe_t * subscr_ptr;
  struct port * source_port_ptr;
  struct port * dest_port_ptr;

  snd_seq_port_subscribe_alloca(&subscr_ptr);

  source_port_ptr = find_port_by_index(&g_input_ports, index_source);
  if (source_port_ptr == NULL)
    return "Source index wrong!!!";

  dest_port_ptr = find_port_by_index(&g_output_ports, index_dest);
  if (dest_port_ptr == NULL)
    return "Dest index wrong!!!";

  snd_seq_port_subscribe_set_sender(subscr_ptr, snd_seq_port_info_get_addr(source_port_ptr->pinfo_ptr));
  snd_seq_port_subscribe_set_dest(subscr_ptr, snd_seq_port_info_get_addr(dest_port_ptr->pinfo_ptr));
  snd_seq_port_subscribe_set_queue(subscr_ptr, 0);
  snd_seq_port_subscribe_set_exclusive(subscr_ptr, 0);
  snd_seq_port_subscribe_set_time_update(subscr_ptr, 0);
  snd_seq_port_subscribe_set_time_real(subscr_ptr, 0);

  if (snd_seq_subscribe_port(seq_handle, subscr_ptr) < 0)
    return "snd_seq_subscribe_port() failed.";

  return NULL;
}

int main()
{
  int ret;
  snd_seq_t * seq_handle;
  int rows, cols;
  struct window windows[3];
  int ch;
  int window_selection;
  WINDOW * help_window;
  const char * err_message;

  INIT_LIST_HEAD(&g_input_ports);
  INIT_LIST_HEAD(&g_output_ports);
  INIT_LIST_HEAD(&g_connections);

  ret = snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_DUPLEX, 0);
  if (ret < 0)
  {
    ERR_OUT("Cannot open sequencer handle - %s", snd_strerror(ret));
    ret = 1;
    goto exit;
  }

  /* set our name (otherwise it's "Client-xxx") */
  ret = snd_seq_set_client_name(seq_handle, "naconnect");
  if (ret < 0)
  {
    ERR_OUT("Cannot set client name - %s", snd_strerror(ret));
    ret = -1;
    goto close_sequencer;
  }

  build_ports(seq_handle);
  build_connections(seq_handle);

  initscr();
  noecho();

  if (has_colors() == FALSE)
  {
    ERR_OUT("Your terminal does not support color");
    ret = -1;
    goto quit;
  }

  start_color();
  init_pair(1, COLOR_CYAN, COLOR_BLACK);
  init_pair(2, COLOR_BLACK, COLOR_WHITE);
  init_pair(3, COLOR_BLACK, COLOR_GREEN);
  init_pair(4, COLOR_WHITE, COLOR_BLACK);
  init_pair(5, COLOR_BLACK, COLOR_RED);
  init_pair(6, COLOR_YELLOW, COLOR_BLACK);

  getmaxyx(stdscr, rows, cols);

  create_ports_win(windows, &g_input_ports, rows/2, cols/2, 0, 0, "Inputs");
  create_ports_win(windows+1, &g_output_ports, rows/2, cols - cols/2, 0, cols/2, "Outputs");
  create_connections_win(windows+2, rows-rows/2-1, cols, rows/2, 0);
  help_window = newwin(0, cols, rows-1, 0);

  window_selection = 0;
  windows[window_selection].selected = 1;

  curs_set(0);                  /* set cursor invisible */

  err_message = NULL;

loop:
  draw_ports(windows);
  draw_ports(windows+1);
  draw_connections(windows+2);

  if (err_message != NULL)
  {
    wattron(help_window, COLOR_PAIR(5));
    mvwprintw(help_window, 0, 1, "%s", err_message);
    wattroff(help_window, COLOR_PAIR(5));
    err_message = NULL;
  }
  else
  {
    wattron(help_window, COLOR_PAIR(6));
    mvwprintw(help_window, 0, 1, "'q'uit, ARROWS-selection, TAB-focus, 'r'efreshe, 'c'onnect, 'd'isconnect");
    wattroff(help_window, COLOR_PAIR(6));
  }

  wclrtoeol(help_window);

  wrefresh(help_window);

  wmove(windows[0].window_ptr, 0, 0);

  keypad(windows[0].window_ptr, TRUE);
  ch = wgetch(windows[0].window_ptr);

  if (ch == '\t')
  {
    windows[window_selection].selected = 0;
    window_selection = (window_selection + 1) % 3;
    if (window_selection == 2 && list_empty(&g_connections))
      window_selection = 0;
    windows[window_selection].selected = 1;
    goto loop;
  }

  if (ch == 'q')
    goto quit;

  if (ch == 'r')
    goto refresh;

  if (ch == 'c')
  {
    err_message = connect(seq_handle, windows[0].index, windows[1].index);
    if (err_message != NULL)
      goto loop;

    goto refresh;
  }

  if (ch == 'd')
  {
    if (disconnect(seq_handle, windows[2].index) < 0)
    {
      err_message = "Disconnection failed";
      goto loop;
    }

    goto refresh;
  }

  if (window_selection < 2)
  {
    ports_handle_key(windows+window_selection, ch);
  }
  else
  {
    connections_handle_key(windows+2, ch);
  }

  goto loop;

refresh:
  free_connections();
  free_all_ports();

  build_ports(seq_handle);
  build_connections(seq_handle);

  items_count(windows);
  items_count(windows+1);
  items_count(windows+2);

  wclear(windows[0].window_ptr);
  wclear(windows[1].window_ptr);
  wclear(windows[2].window_ptr);

  goto loop;

quit:
  endwin();

  ret = 0;

  free_all_ports();

close_sequencer:
  snd_seq_close(seq_handle);

exit:
  return ret;
}
