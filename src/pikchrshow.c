/*
** Copyright (c) 2020 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file contains fossil-specific code related to pikchr.
*/
#include "config.h"
#include <assert.h>
#include <ctype.h>
#include "pikchrshow.h"

#if INTERFACE
/* These are described in pikchr_process()'s docs. */
#define PIKCHR_PROCESS_TH1        0x01
#define PIKCHR_PROCESS_TH1_NOSVG  0x02
#define PIKCHR_PROCESS_NONCE      0x04
#define PIKCHR_PROCESS_ERR_PRE    0x08
#define PIKCHR_PROCESS_SRC        0x10
#define PIKCHR_PROCESS_DIV        0x20
#define PIKCHR_PROCESS_DIV_INDENT        0x0100
#define PIKCHR_PROCESS_DIV_CENTER        0x0200
#define PIKCHR_PROCESS_DIV_FLOAT_LEFT    0x0400
#define PIKCHR_PROCESS_DIV_FLOAT_RIGHT   0x0800
#define PIKCHR_PROCESS_DIV_TOGGLE        0x1000
#define PIKCHR_PROCESS_DIV_SOURCE        0x2000
#define PIKCHR_PROCESS_DIV_SOURCE_INLINE 0x4000
#endif

/*
** Processes a pikchr script, optionally with embedded TH1, and
** produces HTML code for it. zIn is the NUL-terminated input
** script. pikFlags may be a bitmask of any of the PIKCHR_PROCESS_xxx
** flags documented below. thFlags may be a bitmask of any of the
** TH_INIT_xxx and/or TH_R2B_xxx flags. Output is sent to pOut,
** appending to it without modifying any prior contents.
**
** Returns 0 on success, 1 if TH1 processing failed, or 2 if pikchr
** processing failed. In either case, the error message (if any) from
** TH1 or pikchr will be appended to pOut.
**
** pikFlags flag descriptions:
**
** - PIKCHR_PROCESS_TH1 means to run zIn through TH1, using the TH1
** init flags specified in the 3rd argument. If thFlags is non-0 then
** this flag is assumed even if it is not specified.
**
** - PIKCHR_PROCESS_TH1_NOSVG means that processing stops after the
** TH1 eval step, thus the output will be (presumably) a
** TH1-generated/processed pikchr script (or whatever else the TH1
** outputs). If this flag is set, PIKCHR_PROCESS_TH1 is assumed even
** if it is not specified.
**
** All of the remaining flags listed below are ignored if
** PIKCHR_PROCESS_TH1_NOSVG is specified!
**
** - PIKCHR_PROCESS_DIV: if set, the SVG result is wrapped in a DIV
** element which specifies a max-width style value based on the SVG's
** calculated size. This flag has multiple mutually exclusive forms:
**
**  - PIKCHR_PROCESS_DIV uses default element alignment.
**  - PIKCHR_PROCESS_DIV_INDENT indents the div.
**  - PIKCHR_PROCESS_DIV_CENTER centers the div.
**  - PIKCHR_PROCESS_DIV_FLOAT_LEFT floats the div left.
**  - PIKCHR_PROCESS_DIV_FLOAT_RIGHT floats the div right.
**
** If more than one is specified, which one is used is undefined. Those
** flags may be OR'd with one or both of the following:
**
**  - PIKCHR_PROCESS_DIV_TOGGLE: adds the 'toggle' CSS class to the
**    outer DIV so that event-handler code can install different
**    toggling behaviour than the default. Default is ctrl-click, but
**    this flag enables single-click toggling for the element.
**
**  - PIKCHR_PROCESS_DIV_SOURCE: adds the 'source' CSS class to the
**    outer DIV, which is a hint to the client-side renderer (see
**    fossil.pikchr.js) that the pikchr should initially be rendered
**    in source code form mode (the default is to hide the source and
**    show the SVG).
**
**  - PIKCHR_PROCESS_DIV_SOURCE_INLINE: adds the 'source-inline' CSS
**    class to the outer wrapper. This modifier changes how the
**    'source' CSS class gets applied: with this flag, the source view
**    should be rendered "inline" (same position as the graphic), else
**    it is to be left-aligned.
**
** - PIKCHR_PROCESS_NONCE: if set, the resulting SVG/DIV are wrapped
** in "safe nonce" comments, which are a fossil-internal mechanism
** which prevents the wiki/markdown processors from re-processing this
** output. This is necessary when calling this routine in the context
** of wiki/embedded doc processing, but not (e.g.) when fetching
** an image for /pikchrpage.
**
** - PIKCHR_PROCESS_SRC: if set, a new PRE.pikchr-src element is
** injected adjacent to the SVG element which contains the
** HTML-escaped content of the input script. If
** PIKCHR_PROCESS_DIV_SOURCE or PIKCHR_PROCESS_DIV_SOURCE_INLINE is
** set, this flag is automatically implied.
**
** - PIKCHR_PROCESS_ERR_PRE: if set and pikchr() fails, the resulting
** error report is wrapped in a PRE element, else it is retained
** as-is (intended only for console output).
*/
int pikchr_process(const char * zIn, int pikFlags, int thFlags,
                   Blob * pOut){
  Blob bIn = empty_blob;
  int isErr = 0;
  const char *zNonce = (PIKCHR_PROCESS_NONCE & pikFlags)
    ? safe_html_nonce(1) : 0;

  if(!(PIKCHR_PROCESS_DIV & pikFlags)
     /* If any DIV_xxx flags are set, set DIV */
     && (PIKCHR_PROCESS_DIV_INDENT
         | PIKCHR_PROCESS_DIV_CENTER
         | PIKCHR_PROCESS_DIV_FLOAT_RIGHT
         | PIKCHR_PROCESS_DIV_FLOAT_LEFT
         | PIKCHR_PROCESS_DIV_SOURCE
         | PIKCHR_PROCESS_DIV_SOURCE_INLINE
         | PIKCHR_PROCESS_DIV_TOGGLE
         ) & pikFlags){
    pikFlags |= PIKCHR_PROCESS_DIV;
  }
  if(!(PIKCHR_PROCESS_TH1 & pikFlags)
     /* If any TH1_xxx flags are set, set TH1 */
     && (PIKCHR_PROCESS_TH1_NOSVG & pikFlags || thFlags!=0)){
    pikFlags |= PIKCHR_PROCESS_TH1;
  }  
  if(zNonce){
    blob_appendf(pOut, "%s\n", zNonce);
  }
  if(PIKCHR_PROCESS_TH1 & pikFlags){
    Blob out = empty_blob;
    isErr = Th_RenderToBlob(zIn, &out, thFlags)
      ? 1 : 0;
    if(isErr){
      blob_append(pOut, blob_str(&out), blob_size(&out));
      blob_reset(&out);
    }else{
      bIn = out;
    }
  }else{
    blob_init(&bIn, zIn, -1);
  }
  if(!isErr){
    if(PIKCHR_PROCESS_TH1_NOSVG & pikFlags){
      blob_append(pOut, blob_str(&bIn), blob_size(&bIn));
    }else{
      int w = 0, h = 0;
      const char * zContent = blob_str(&bIn);
      char *zOut;
      zOut = pikchr(zContent, "pikchr",
                    0x01/*==>PIKCHR_PLAINTEXT_ERRORS*/,
                    &w, &h);
      if( w>0 && h>0 ){
        const char * zClassToggle = "";
        const char * zClassSource = "";
        const char * zWrapperClass = "";
        if(PIKCHR_PROCESS_DIV & pikFlags){
          if(PIKCHR_PROCESS_DIV_CENTER & pikFlags){
            zWrapperClass = " center";
          }else if(PIKCHR_PROCESS_DIV_INDENT & pikFlags){
            zWrapperClass = " indent";
          }else if(PIKCHR_PROCESS_DIV_FLOAT_LEFT & pikFlags){
            zWrapperClass = " float-left";
          }else if(PIKCHR_PROCESS_DIV_FLOAT_RIGHT & pikFlags){
            zWrapperClass = " float-right";
          }
          if(PIKCHR_PROCESS_DIV_TOGGLE & pikFlags){
            zClassToggle = " toggle";
          }
          if(PIKCHR_PROCESS_DIV_SOURCE_INLINE & pikFlags){
            if(PIKCHR_PROCESS_DIV_SOURCE & pikFlags){
              zClassSource = " source source-inline";
            }else{
              zClassSource = " source-inline";
            }
            pikFlags |= PIKCHR_PROCESS_SRC;
          }else if(PIKCHR_PROCESS_DIV_SOURCE & pikFlags){
            zClassSource = " source";
            pikFlags |= PIKCHR_PROCESS_SRC;
          }
          blob_appendf(pOut,"<div class='pikchr-wrapper"
                       "%s%s%s'>"
                       "<div class=\"pikchr-svg\" "
                       "style=\"max-width:%dpx\">\n",
                       zWrapperClass/*safe-for-%s*/,
                       zClassToggle/*safe-for-%s*/,
                       zClassSource/*safe-for-%s*/, w);
        }
        blob_append(pOut, zOut, -1);
        if(PIKCHR_PROCESS_DIV & pikFlags){
          blob_append(pOut, "</div>\n", 7);
        }
        if(PIKCHR_PROCESS_SRC & pikFlags){
          blob_appendf(pOut, "<pre class='pikchr-src'>%h</pre>\n",
                       blob_str(&bIn));
        }
        if(PIKCHR_PROCESS_DIV & pikFlags){
          blob_append(pOut, "</div>\n", 7);
        }
      }else{
        isErr = 2;
        if(PIKCHR_PROCESS_ERR_PRE & pikFlags){
          blob_append(pOut, "<pre class='error'>\n", 20);
        }
        blob_append(pOut, zOut, -1);
        if(PIKCHR_PROCESS_ERR_PRE & pikFlags){
          blob_append(pOut, "\n</pre>\n", 8);
        }
      }
      fossil_free(zOut);
    }
  }
  if(zNonce){
    blob_appendf(pOut, "%s\n", zNonce);
  }
  blob_reset(&bIn);
  return isErr;
}

/*
** WEBPAGE: pikchrshow
**
** A pikchr code editor and previewer, allowing users to experiment
** with pikchr code or prototype it for use in copy/pasting into forum
** posts, wiki pages, or embedded docs.
**
** It optionally accepts a p=pikchr-script-code URL parameter or POST
** value to pre-populate the editor with that code.
*/
void pikchrshow_page(void){
  const char *zContent = 0;
  int isDark;              /* true if the current skin is "dark" */
  int pikFlags =
    PIKCHR_PROCESS_DIV
    | PIKCHR_PROCESS_SRC
    | PIKCHR_PROCESS_ERR_PRE;

  login_check_credentials();
  if( !g.perm.RdWiki && !g.perm.Read && !g.perm.RdForum ){
    cgi_redirectf("%R/login?g=%R/pikchrshow");
  }
  zContent = PD("content",P("p"));
  if(P("ajax")!=0){
    /* Called from the JS-side preview updater.
       TODO: respond with JSON instead.*/
    cgi_set_content_type("text/html");
    if(zContent && *zContent){
      Blob out = empty_blob;
      const int isErr =
        pikchr_process(zContent, pikFlags, 0, &out);
      if(isErr){
        cgi_printf_header("x-pikchrshow-is-error: %d\r\n", isErr);
      }
      CX("%b", &out);
      blob_reset(&out);
    }else{
      CX("<pre>No content! Nothing to render</pre>");
    }
    return;
  }/*ajax response*/
  style_emit_noscript_for_js_page();
  isDark = skin_detail_boolean("white-foreground");
  if(!zContent){
    zContent = "arrow right 200% \"Markdown\" \"Source\"\n"
      "box rad 10px \"Markdown\" \"Formatter\" \"(markdown.c)\" fit\n"
      "arrow right 200% \"HTML+SVG\" \"Output\"\n"
      "arrow <-> down from last box.s\n"
      "box same \"Pikchr\" \"Formatter\" \"(pikchr.c)\" fit\n";
  }
  style_header("PikchrShow");
  CX("<style>"); {
    CX("div.content { padding-top: 0.5em }\n");
    CX("#sbs-wrapper {"
       "display: flex; flex-direction: column;"
       "}\n");
    CX("#sbs-wrapper > * {"
       "margin: 0 0.25em 0.5em 0; flex: 1 10 auto;"
       "align-self: stretch;"
       "}\n");
    CX("#sbs-wrapper textarea {"
       "max-width: initial; flex: 1 1 auto;"
       "}\n");
    CX("#pikchrshow-output, #pikchrshow-form"
       "{display: flex; flex-direction: column; align-items: stretch;}");
    CX("#pikchrshow-form > * {margin: 0.25em 0}\n");
    CX("#pikchrshow-output {flex: 5 1 auto; padding: 0}\n");
    CX("#pikchrshow-output > pre, "
       "#pikchrshow-output > pre > div, "
       "#pikchrshow-output > pre > div > pre "
       "{margin: 0; padding: 0}\n");
    CX("#pikchrshow-output.error > pre "
       /* Server-side error report */
       "{padding: 0.5em}\n");
    CX("#pikchrshow-controls {" /* where the buttons live */
       "display: flex; flex-direction: row; "
       "align-items: center; flex-wrap: wrap;"
       "}\n");
    CX("#pikchrshow-controls > * {"
       "display: inline; margin: 0 0.25em 0.5em 0;"
       "}\n");
    CX("#pikchrshow-output-wrapper label {"
       "cursor: pointer;"
       "}\n");
    CX("body.pikchrshow .input-with-label > * {"
       "margin: 0 0.2em;"
       "}\n");
    CX("body.pikchrshow .input-with-label > label {"
       "cursor: pointer;"
       "}\n");
    CX("#pikchrshow-output.dark-mode svg {"
       /* Flip the colors to approximate a dark theme look */
       "filter: invert(1) hue-rotate(180deg);"
       "}\n");
    CX("#pikchrshow-output-wrapper {"
       "padding: 0.25em 0.5em; border-radius: 0.25em;"
       "border-width: 1px;"/*some skins disable fieldset borders*/
       "}\n");
    CX("#pikchrshow-output-wrapper > legend > *:not(.copy-button){"
       "margin-right: 0.5em; vertical-align: middle;"
       "}\n");
    CX("body.pikchrshow .v-align-middle{"
       "vertical-align: middle"
       "}\n");
    CX(".dragover {border: 3px dotted rgba(0,255,0,0.6)}\n");
  } CX("</style>");
  CX("<div>Input pikchr code and tap Preview or Ctrl-Enter) to render "
     "it:</div>");
  CX("<div id='sbs-wrapper'>"); {
    CX("<div id='pikchrshow-form'>"); {
      CX("<textarea id='content' name='content' rows='15'>"
         "%s</textarea>",zContent/*safe-for-%s*/);
      CX("<div id='pikchrshow-controls'>"); {
        CX("<button id='pikchr-submit-preview'>Preview</button>");
        CX("<div class='input-with-label'>"); {
          CX("<button id='pikchr-stash'>Stash</button>");
          CX("<button id='pikchr-unstash'>Unstash</button>");
          CX("<button id='pikchr-clear-stash'>Clear stash</button>");
          CX("<span>Stores/restores a single pikchr script to/from "
             "browser-local storage from/to the editor.</span>"
             /* gets turned into a help-buttonlet */);
        } CX("</div>"/*stash controls*/);
        style_labeled_checkbox("flipcolors-wrapper", "flipcolors",
                               "Dark mode?",
                               "1", isDark, 0);
      } CX("</div>"/*#pikchrshow-controls*/);
    }
    CX("</div>"/*#pikchrshow-form*/);
    CX("<fieldset id='pikchrshow-output-wrapper'>"); {
      CX("<legend></legend>"
         /* Reminder: Firefox does not properly flexbox a LEGEND
            element, always flowing it in column mode. */);
      CX("<div id='pikchrshow-output'>");
      if(*zContent){
        Blob out = empty_blob;
        pikchr_process(zContent, pikFlags, 0, &out);
        CX("%b", &out);
        blob_reset(&out);
      } CX("</div>"/*#pikchrshow-output*/);
    } CX("</fieldset>"/*#pikchrshow-output-wrapper*/);
  } CX("</div>"/*sbs-wrapper*/);
  builtin_fossil_js_bundle_or("fetch", "copybutton", "popupwidget",
                              "storage", "pikchr", 0);
  builtin_request_js("fossil.page.pikchrshow.js");
  builtin_fulfill_js_requests();
  style_footer();
}

/*
** COMMAND: pikchr*
**
** Usage: %fossil pikchr [options] ?INFILE? ?OUTFILE?
**
** Accepts a pikchr script as input and outputs the rendered script as
** an SVG graphic. The INFILE and OUTFILE options default to stdin
** resp. stdout, and the names "-" can be used as aliases for those
** streams.
**
** Options:
**
**    -div       On success, adds a DIV wrapper around the
**               resulting SVG output which limits its max-width to
**               its computed maximum ideal size.
**
**    -div-indent  Like -div but indents the div.
**
**    -div-center  Like -div but centers the div.
**
**    -div-left    Like -div but floats the div left.
**
**    -div-right   Like -div but floats the div right.
**
**    -div-toggle  Sets the 'toggle' CSS class on the div (used by the
**                 JavaScript-side post-processor).
**
**    -div-source  Sets the 'source' CSS class on the div, which tells
**                 CSS to hide the SVG and reveal the source by default.
**
**    -src       Stores the input pikchr's source code in the output as
**               a separate element adjacent to the SVG one. Implied
**               by -div-source.
**                
**
**    -th        Process the input using TH1 before passing it to pikchr.
**
**    -th-novar  Disable $var and $<var> TH1 processing. Use this if the
**               pikchr script uses '$' for its own purposes and that
**               causes issues. This only affects parsing of '$' outside
**               of TH1 script blocks. Code in such blocks is unaffected.
**
**    -th-nosvg  When using -th, output the post-TH1'd script
**               instead of the pikchr-rendered output.
**
**    -th-trace  Trace TH1 execution (for debugging purposes).
**
**
** The -div-indent/center/left/right flags may not be combined.
**
** TH1-related Notes and Caveats:
**
** If the -th flag is used, this command must open a fossil database
** for certain functionality to work (via a checkout or the -R REPO
** flag). If opening a db fails, execution will continue but any TH1
** commands which require a db will trigger a fatal error.
**
** In Fossil skins, TH1 variables in the form $varName are expanded
** as-is and those in the form $<varName> are htmlized in the
** resulting output. This processor disables the htmlizing step, so $x
** and $<x> are equivalent unless the TH1-processed pikchr script
** invokes the TH1 command [enable_htmlify 1] to enable it. Normally
** that option will interfere with pikchr output, however, e.g. by
** HTML-encoding double-quotes.
**
** Many of the fossil-installed TH1 functions simply do not make any
** sense for pikchr scripts.
*/
void pikchr_cmd(void){
  Blob bIn = empty_blob;
  Blob bOut = empty_blob;
  const char * zInfile = "-";
  const char * zOutfile = "-";
  const int fTh1 = find_option("th",0,0)!=0;
  const int fNosvg = find_option("th-nosvg",0,0)!=0;
  int isErr = 0;
  int pikFlags = find_option("src",0,0)!=0
    ? PIKCHR_PROCESS_SRC : 0;
  u32 fThFlags = TH_INIT_NO_ENCODE
    | (find_option("th-novar",0,0)!=0 ? TH_R2B_NO_VARS : 0);

  Th_InitTraceLog()/*processes -th-trace flag*/;

  if(find_option("div",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV;
  }else if(find_option("div-indent",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV_INDENT;
  }else if(find_option("div-center",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV_CENTER;
  }else if(find_option("div-float-left",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV_FLOAT_LEFT;
  }else if(find_option("div-float-right",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV_FLOAT_RIGHT;
  }
  if(find_option("div-toggle",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV_TOGGLE;
  }
  if(find_option("div-source",0,0)!=0){
    pikFlags |= PIKCHR_PROCESS_DIV_SOURCE | PIKCHR_PROCESS_SRC;
  }

  verify_all_options();
  if(g.argc>4){
    usage("?INFILE? ?OUTFILE?");
  }
  if(g.argc>2){
    zInfile = g.argv[2];
  }
  if(g.argc>3){
    zOutfile = g.argv[3];
  }
  blob_read_from_file(&bIn, zInfile, ExtFILE);
  if(fTh1){
    db_find_and_open_repository(OPEN_ANY_SCHEMA | OPEN_OK_NOT_FOUND, 0)
      /* ^^^ needed for certain TH1 functions to work */;
    pikFlags |= PIKCHR_PROCESS_TH1;
    if(fNosvg) pikFlags |= PIKCHR_PROCESS_TH1_NOSVG;
  }
  isErr = pikchr_process(blob_str(&bIn), pikFlags,
                         fTh1 ? fThFlags : 0, &bOut);
  if(isErr){
    fossil_fatal("%s ERROR:%c%b", 1==isErr ? "TH1" : "pikchr",
                 1==isErr ? ' ' : '\n',
                 &bOut);
  }else{
    blob_write_to_file(&bOut, zOutfile);
  }
  Th_PrintTraceLog();
  blob_reset(&bIn);
  blob_reset(&bOut);
}
