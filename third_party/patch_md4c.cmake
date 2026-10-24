# Patches md4c's HTML renderer, run as FetchContent's PATCH_COMMAND in md4c's folder, so that
# what std.text.markdown's tree has is what its HTML has: a numeric reference to a surrogate is
# U+FFFD, as CommonMark makes any code point that isn't one, and a code span in a table's cell
# takes "\|" as a '|', as GitHub's tables do. Each change is made once, and running it again
# changes nothing.

set(file "src/md4c-html.c")
file(READ "${file}" text)

# Replaces `old` with `new` in text, where it hasn't already; `old` has to be there once.
function(patch old new)
  string(FIND "${text}" "${new}" done)
  if(NOT done EQUAL -1)
    return()
  endif()

  string(FIND "${text}" "${old}" first)
  string(FIND "${text}" "${old}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR "md4c's ${file} doesn't have what its patch changes once: ${old}")
  endif()

  string(REPLACE "${old}" "${new}" text "${text}")
  set(text "${text}" PARENT_SCOPE)
endfunction()

patch([=[    if(0 < codepoint  &&  codepoint <= 0x10ffff)]=]
      [=[    if(0 < codepoint  &&  codepoint <= 0x10ffff  &&  (codepoint < 0xd800  ||  codepoint > 0xdfff))]=])

patch([=[    char escape_map[256];
};]=]
      [=[    char escape_map[256];
    int in_cell;
};]=])
patch([=[        case MD_BLOCK_TH:       render_open_td_block(r, "th", (MD_BLOCK_TD_DETAIL*)detail); break;
        case MD_BLOCK_TD:       render_open_td_block(r, "td", (MD_BLOCK_TD_DETAIL*)detail); break;]=]
      [=[        case MD_BLOCK_TH:       r->in_cell = 1; render_open_td_block(r, "th", (MD_BLOCK_TD_DETAIL*)detail); break;
        case MD_BLOCK_TD:       r->in_cell = 1; render_open_td_block(r, "td", (MD_BLOCK_TD_DETAIL*)detail); break;]=])
patch([=[        case MD_BLOCK_TH:       RENDER_VERBATIM(r, "</th>\n"); break;
        case MD_BLOCK_TD:       RENDER_VERBATIM(r, "</td>\n"); break;]=]
      [=[        case MD_BLOCK_TH:       r->in_cell = 0; RENDER_VERBATIM(r, "</th>\n"); break;
        case MD_BLOCK_TD:       r->in_cell = 0; RENDER_VERBATIM(r, "</td>\n"); break;]=])
patch([=[static int
text_callback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)]=]
      [=[/* A code span's text in a table cell, with each "\|" as a '|'. */
static void
render_cell_code(MD_HTML* r, const MD_CHAR* text, MD_SIZE size)
{
    MD_SIZE beg = 0;
    MD_SIZE off;

    for(off = 0; off + 1 < size; off++) {
        if(text[off] == '\\'  &&  text[off+1] == '|') {
            render_html_escaped(r, text + beg, off - beg);
            beg = off + 1;
        }
    }

    render_html_escaped(r, text + beg, size - beg);
}

static int
text_callback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)]=])
patch([=[        default:                render_html_escaped(r, text, size); break;]=]
      [=[        case MD_TEXT_CODE:      if(r->in_cell)
                                    render_cell_code(r, text, size);
                                else
                                    render_html_escaped(r, text, size);
                                break;
        default:                render_html_escaped(r, text, size); break;]=])

file(WRITE "${file}" "${text}")
