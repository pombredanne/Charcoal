<html>
<head>
<title>Tight Loop Examples</title>
<link rel="stylesheet" media="screen" type="text/css" href="charcoal.css"/>
</head>
<body style="background-color:darkgray">

<?php include 'code_examples.php'; ?>

<?php include 'side_bar_examples.html'; ?>

<div class="main_div">

<h1>Tight Loop Examples</h1>

<p>Yielding in Charcoal is quite cheap, but it's not free.  For the sake
of back-of-the-envelope performance estimates, you can assume a yield
that immediately returns back to the current activity costs maybe a
dozen instructions.  In most code this overhead shouldn't be worth
thinking about, but in tight inner loops it can be painful.  For
example:</p>

<?php format_code(
'<i>float</i> <b>dot_product</b>( <i>size_t</i> <b>N</b>, <i>float *</i><b>A</b>, <i>float *</i><b>B</b> )
{
    <i>float</i> <b>rv</b> = 0.0;
    <i>size_t</i> <b>i</b>;
    <b><u>for</u></b>( i = 0; i &lt; N; ++i )
    {
        rv += A[i] * b[i];
    }
    <b><u>return</u></b> rv;
}' ); ?>

<p>The inner loop here is probably well under a dozen instructions, so
adding a yield on every iteration (which happens by default in Charcoal)
more than doubles the run time.  If someone cares about the performance
of this code at all, that's probably unacceptable.  The simplest "fix"
for this problem is to use the for loop variant that does not yield by
default.</p>

<?php format_code(
'<i>float</i> <b>dot_product</b>( <i>size_t</i> <b>N</b>, <i>float *</i><b>A</b>, <i>float *</i><b>B</b> )
{
    <i>float</i> <b>rv</b> = 0.0;
    <i>size_t</i> <b>i</b>;
    <b><u><span title="Look ma, no yielding!" class="yellow">for_no_yield</span></u></b>( i = 0; i &lt; N; ++i )
    {
        rv += A[i] * b[i];
    }
    <b><u>return</u></b> rv;
}' ); ?>

<p>This version avoids yielding too frequently, but if it is ever called
with a large input, it may not yield frequently enough.
Well-behaved Charcoal programs should go no more than a few milliseconds
between yields.</p>

<p>Hitting a sweet spot in terms of yield frequency, while adding zero
overhead to the inner loop requires somewhat more complex code.</p>

<?php format_code(
'<b><u>#define</u></b> <b>BLOCK_SIZE</b> 32

<i>float</i> <b>dot_product</b>( <i>size_t</i> <b>N</b>, <i>float *</i><b>A</b>, <i>float *</i><b>B</b> )
{
    <i>float</i> <b>rv</b> = 0.0;
    <i>size_t</i> <b>i</b> = 0, <b>j</b>;
    <b><u>for</u></b>( j = BLOCK_SIZE; j &lt; N; j += BLOCK_SIZE )
    {
        <b><u>for_no_yield</u></b>( ; i &lt; j; ++i )
        {
            rv += A[i] * b[i];
        }
    }
    <b><u>for_no_yield</u></b>( ; i &lt; N; ++i )
    {
        rv += A[i] * b[i];
    }
    <b><u>return</u></b> rv;
}' ); ?>

<p>[Some explanatory text here]</p>

<p>(Bug hunter bonus points for anyone who noticed there is a (probably
extremely rare) overflow bug when
<span class="mono">N</span> &gt;
<span class="mono">SIZE_MAX</span>
- <span class="mono">BLOCK_SIZE</span>.  This shouldn't
be a complex bug to fix.)</p>

<p>Things get a little trickier if the loop condition is
data-dependent.</p>

<?php format_code(
'<b><u>#define</u></b> <b>BLOCK_SIZE</b> 32

<i>char *</i><b>mystrcpy</b>( <i>char *</i><b>dest</b>, <i>const char *</i><b>src</b> )
{
    <i>char *</i><b>rv</b> = dst;
    <b><u>while_no_yield</u></b>( *dst++ = *src++ )
        <b><u>if</u></b>( 0 == dst % BLOCK_SIZE )
            <b><u>yield</u></b>;
    <b><u>return</u></b> rv;
}' ); ?>

<p>This example is just slightly less satisfying than the previous one
performance-wise, because we had to add an extra branch in the inner
loop.  There is no easy way around this, because termination of the loop
might happen at any time.  On the other hand, high performance
implementations of strcpy are usually quite architecture-dependent, so
it might be possible to sneak the periodic yield in to one of those with
no performance ill effects.</p>

<p>This example also nicely illustrates that
the <span class="mono">X_no_yield</span> statements
(where <span class="mono">X</span> could
be <span class="mono">for</span>, <span class="mono">while</span>,
<span class="mono">goto</span>, ...) are distinct from
the <span class="mono">unyielding</span> keyword.  The "no yield"
variants simply tell the compiler not to insert the implicit yields that
it would for the "normal" variants.
The <span class="mono">while_no_yield</span> on line 6 above does not
interfere with the explcit <span class="mono">yield</span> on line
8.</p>

<h2>Conclusion</h2>

<p>Most Charcoal code shouldn't need tricks like this.  Most
applications have only a few performance-critical inner loops (if they
are CPU-bound at all).  For those tight inner loops, Charcoal does
impose a little code complexity penalty to make sure yields happen at a
good rate.  However, the penalty is not terribly high.  The story is a
little more complex for library code, and that is addressed in a
different example.</p>

<?php include 'copyright.html'; ?>

</div>
</body>
</html>
