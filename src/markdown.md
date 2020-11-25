# Markdown Overview #

## Paragraphs ##

> Paragraphs are divided by blank lines.  
> End a line with two or more spaces to force a mid-paragraph line break.

## Headings ##

>
    # Top Level Heading                 Alternative Top Level Heading
    # Top Level Heading Variant #       =============================
>
    ## 2nd Level Heading                Alternative 2nd Level Heading
    ## 2nd Level Heading Variant ##     -----------------------------
>
    ### 3rd Level Heading               ### 3rd Level Heading Variant ###
    #### 4th Level Heading              #### 4th Level Heading Variant ####
    ##### 5th Level Heading             ##### 5th Level Heading Variant #####
    ###### 6th Level Heading            ###### 6th Level Heading Variant ######

## Links ##

> 1.  **\[display text\]\(URL\)**
> 2.  **\[display text\]\(URL "Title"\)**
> 3.  **\[display text\]\(URL 'Title'\)**
> 4.  **\<URL\>**
> 5.  **\[display text\]\[label\]**
> 6.  **\[display text\]\[\]**
> 7.  **\[display text\]**
> 8.  **\[\]\(URL\)**

> With link formats 5, 6, and 7 ("reference links"), the URL is supplied
> elsewhere in the document, as shown below.  Link formats 6 and 7 reuse
> the display text as the label.  Labels are case-insensitive.  The title
> may be split onto the next line with optional indenting.

> * **\[label\]:&nbsp;URL**
> * **\[label\]:&nbsp;URL&nbsp;"Title"**
> * **\[label\]:&nbsp;URL&nbsp;'Title'**
> * **\[label\]:&nbsp;URL&nbsp;(Title)**

> If **URL** begins with "http:", "https:', "ftp:' or "mailto:",
> it may optionally be written **\<URL\>** (format 4).
> Other **URL** formats include:
> <ul>
> <li>  A relative pathname.
> <li>  A pathname starting with "/" in which case the Fossil server
>       URL prefix is prepended
> <li>  A wiki page name, or a wiki page name preceded by "wiki:"
> <li>  An artifact or ticket hash or hash prefix
> <li>  A date and time stamp: "YYYY-MM-DD HH:MM:SS" or a subset that
>       includes at least the day of the month.
> <li>  An [interwiki link](#intermap) of the form "<i>Tag</i><b>:</b><i>PageName</i>"</ul>

> In format 8, then the URL becomes the display text.  This is useful for
> hyperlinks that refer to wiki pages and check-in and ticket hashes.

## Fonts ##

> *   _\*italic\*_
> *   *\_italic\_*
> *   __\*\*bold\*\*__
> *   **\_\_bold\_\_**
> *   ___\*\*\*italic+bold\*\*\*___
> *   ***\_\_\_italic+bold\_\_\_***
> *   \``code`\`

> The **\`code\`** construct disables HTML markup, so one can write, for
> example, **\`\<html\>\`** to yield **`<html>`**.

## Lists ##

>
     *   bullet item
     +   bullet item
     -   bullet item
     1.  numbered item
     2)  numbered item

> A two-level list is created by placing additional whitespace before the
> **\***/**+**/**-**/**1.** of the secondary items.

>
     *   top-level item
       * secondary item

## Block Quotes ##

> Begin each line of a paragraph with **>** to block quote that paragraph.

> >
    > This paragraph is indented
> >
    > > Double-indented paragraph

## Literal/Verbatim Text - Code Blocks ##

> For inline text, you can either use \``backticks`\` or the HTML
> `<code>` tag.
>
> For blocks of text or code:
>
> 1. Indent the text using a tab character or at least four spaces.
> 2. Precede the block with an HTML `<pre>` tag and follow it with `</pre>`.
> 3. Surround the block by <tt>\`\`\`</tt> (three or more) or <tt>\~\~\~</tt> either at the
> left margin or indented no more than three spaces. The first word
> on that same line (if any) is used in a “`language-WORD`” CSS style in
> the HTML rendering of that code block and is intended for use by
> code syntax highlighters. Thus <tt>\`\`\`c</tt> would mark a block of code
> in the C programming language. Text to be rendered inside the code block
> should therefore start on the next line, not be cuddled up with the
> backticks or tildes.  See the "Diagrams" section below for the case where
> "`language-WORD`" is "pikchr".

> With the standard skins, verbatim text is rendered in a fixed-width font,
> but that is purely a presentation matter, controlled by the skin’s CSS.


## Tables ##

>
    | Header 1     | Header 2    | Header 3      |
    ----------------------------------------------
    | Row 1 Col 1  | Row 1 Col 2 | Row 1 Col 3   |
    |:Left-aligned |:Centered   :| Right-aligned:|
    |              | ← Blank   → |               |
    | Row 4 Col 1  | Row 4 Col 2 | Row 4 Col 3   |

> The first row is a header if followed by a horizontal rule or a blank line.

> Placing **:** at the left, both, or right sides of a cell gives left-aligned,
> centered, or right-aligned text, respectively.  By default, header cells are
> centered, and body cells are left-aligned.

> The leftmost or rightmost **\|** is required only if the first or last column,
> respectively, contains at least one blank cell.

## Diagrams ##

>
~~~~~
~~~ pikchr
oval "Start" fit; arrow; box "Hello, World!" fit; arrow; oval "Done" fit
~~~
~~~~~

> Formatted using [Pikchr](https://pikchr.org/home), resulting in:

>
~~~ pikchr
oval "Start" fit; arrow; box "Hello, World!" fit; arrow; oval "Done" fit
~~~

## Miscellaneous ##

> *   In-line images are made using **\!\[alt-text\]\(image-URL\)**.
> *   Use HTML for advanced formatting such as forms.
> *   **\<!--** HTML-style comments **-->** are supported.
> *   Escape special characters (ex: **\[** **\(** **\|** **\***)
>     using backslash (ex: **\\\[** **\\\(** **\\\|** **\\\***).
> *   A line consisting of **---**, **\*\*\***, or **\_\_\_** is a horizontal
>     rule.  Spaces and extra **-**/**\***/**_** are allowed.
> *   See [daringfireball.net][] for additional information.
> *   See this page's [Markdown source](/md_rules?txt=1) for complex examples.

## Special Features For Fossil ##

> *  In hyperlinks, if the URL begins with **/** then the root of the Fossil
>    repository is prepended.  This allows for repository-relative hyperlinks.
> *  For documents that begin with a top-level heading (ex: **# heading #**),
>    the heading is omitted from the body of the document and becomes the
>    document title displayed at the top of the Fossil page.

[daringfireball.net]: http://daringfireball.net/projects/markdown/syntax

<a name="intermap"></a>
## Interwiki Tag Map
