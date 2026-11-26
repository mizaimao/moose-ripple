# Run on the VM host, in the folder of cleanvm.sh. MIT licence (LICENSE at the repository root).
# perl guestctl.pl OS ACTION [ARGS]: keys, text, clicks and pictures for a guest started by
# cleanvm.sh, through its QMP socket.
#   key K            one QEMU key (ret, alt-y, meta_l, ctrl-shift-ret ...)
#   type TEXT        type the text
#   line TEXT        type the text, then Enter
#   run TEXT         Win+R, the text, Enter
#   click X Y W H    left click at pixel X,Y of a W x H screen;  dclick: twice;  rclick: right
#   shot FILE        the screen as FILE (PNG) in this folder; QEMU has no picture once the
#                    driver draws through the host GPU: read the VNC socket then
#                    (tools/vncgrab)
use strict; use warnings; use IO::Socket::UNIX; use JSON::PP; use Time::HiRes qw(usleep); use Cwd qw(getcwd);
my ($os, $act, @a) = @ARGV;
my $s = IO::Socket::UNIX->new(Type => SOCK_STREAM, Peer => "$os-qmp.sock") or die "qmp: $!";
$s->autoflush(1); my $hi = <$s>;
sub qmp {
    my ($name, $args) = @_;
    print $s encode_json({execute => $name, ($args ? (arguments => $args) : ()), id => "rel"}) . "\n";
    while (my $r = <$s>) {
        my $j = decode_json($r);
        next unless ($j->{id} // "") eq "rel";
        die encode_json($j->{error}) if exists $j->{error};
        return $j->{return};
    }
    die "QMP closed";
}
qmp("qmp_capabilities");
sub key { qmp("human-monitor-command", {"command-line" => "sendkey $_[0] 14"}); usleep(45000) }
my %p = (";" => "semicolon", "'" => "apostrophe", "[" => "bracket_left", "]" => "bracket_right",
    "{" => "shift-bracket_left", "}" => "shift-bracket_right", "!" => "shift-1", "@" => "shift-2",
    "#" => "shift-3", "\$" => "shift-4", "+" => "shift-equal", "*" => "shift-8", "?" => "shift-slash",
    "<" => "shift-comma", " " => "spc", "-" => "minus", "_" => "shift-minus", "=" => "equal",
    "/" => "slash", "." => "dot", ":" => "shift-semicolon", "\\" => "backslash", "&" => "shift-7",
    '"' => "shift-apostrophe", ">" => "shift-dot", "%" => "shift-5", "(" => "shift-9", ")" => "shift-0",
    "|" => "shift-backslash", "," => "comma", "^" => "shift-6", "~" => "shift-grave_accent", "`" => "grave_accent");
sub text { foreach (split //, $_[0]) { key(exists $p{$_} ? $p{$_} : /[A-Z]/ ? "shift-" . lc($_) : $_) } }
sub btn {
    my ($b, $x, $y, $w, $h, $n) = @_;
    qmp("input-send-event", {events => [
        {type => "abs", data => {axis => "x", value => int($x * 32767 / ($w - 1))}},
        {type => "abs", data => {axis => "y", value => int($y * 32767 / ($h - 1))}}]});
    usleep(250000);
    for (1 .. $n) {
        qmp("input-send-event", {events => [{type => "btn", data => {down => JSON::PP::true, button => $b}}]});
        usleep(60000);
        qmp("input-send-event", {events => [{type => "btn", data => {down => JSON::PP::false, button => $b}}]});
        usleep(120000);
    }
}
if ($act eq "key") { key($_) for @a }
elsif ($act eq "type") { text($a[0]) }
elsif ($act eq "line") { text($a[0]); key("ret") }
elsif ($act eq "run") { key("meta_l-r"); usleep(1500000); text($a[0]); key("ret") }
elsif ($act eq "click") { btn("left", @a[0 .. 3], 1) }
elsif ($act eq "dclick") { btn("left", @a[0 .. 3], 2) }
elsif ($act eq "rclick") { btn("right", @a[0 .. 3], 1) }
elsif ($act eq "shot") {
    die "name" unless $a[0] =~ /^[a-z0-9.-]+\.png$/;
    qmp("screendump", {filename => getcwd() . "/$a[0]", format => "png"});
}
else { die "action" }
