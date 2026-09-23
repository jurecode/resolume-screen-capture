#include "CaptureTargets.h"
#include "Platform.h"

int FindTarget( const std::vector< CaptureTarget >& targets, const std::string& utf8Text )
{
	std::string needle = platform::LowercaseUtf8( utf8Text );
	if( needle.empty() )
		return -1;

	//Windows first: "chrome" should find the browser, not a monitor that happens to match.
	//Except for "pantalla 2", which should never pick a web page that mentions "pantalla".
	bool monitorFirst = needle.compare( 0, 8, "pantalla" ) == 0;
	CaptureTarget::Kind first  = monitorFirst ? CaptureTarget::Kind::Monitor : CaptureTarget::Kind::Window;
	CaptureTarget::Kind second = monitorFirst ? CaptureTarget::Kind::Window : CaptureTarget::Kind::Monitor;
	for( CaptureTarget::Kind kind : { first, second } )
	{
		for( size_t index = 0; index < targets.size(); ++index )
		{
			if( targets[ index ].kind == kind && targets[ index ].searchKey.find( needle ) != std::string::npos )
				return static_cast< int >( index );
		}
	}
	return -1;
}
